/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "rtmp-publisher.hpp"

#include "rtmp-wire.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/hmac.h>
#include <libavutil/lfg.h>
#include <libavutil/random_seed.h>
#include <libavutil/time.h>
}

namespace trigglow::relay {

namespace {

using namespace rtmp;

constexpr int64_t kSetupTimeoutUs = 15 * 1000000LL;
constexpr const char *kRwTimeoutUs = "15000000";
// How often a sender that keeps writing reads the server's messages.
constexpr int64_t kReadEveryUs = 100 * 1000LL;
// An idle sender reads them this often.
constexpr int64_t kIdleReadEveryUs = 250 * 1000LL;
// FFmpeg's (and Flash's) window acknowledgement size for a publisher.
constexpr uint32_t kOurAckWindow = 2500000;
constexpr int kMuxerIoBytes = 64 * 1024;

// Transaction ids, in the order FFmpeg's client uses them.
constexpr double kTxnConnect = 1;
constexpr double kTxnReleaseStream = 2;
constexpr double kTxnFcPublish = 3;
constexpr double kTxnCreateStream = 4;
constexpr double kTxnPublish = 5;

// Handshake keys: FFmpeg's rtmpproto.c (the first 30 bytes sign C1).
constexpr uint8_t kPlayerKey[] = {'G', 'e', 'n', 'u', 'i', 'n', 'e', ' ', 'A', 'd', 'o', 'b', 'e', ' ', 'F',
				  'l', 'a', 's', 'h', ' ', 'P', 'l', 'a', 'y', 'e', 'r', ' ', '0', '0', '1'};
// Flash Player 9.0.124.2, as FFmpeg announces itself.
constexpr uint8_t kClientVersion[4] = {9, 0, 124, 2};

constexpr const char *kFlashVer = "FMLE/3.0 (compatible; FMSc/1.0)"; // OBS's.

// The head of FFmpeg's URLContext (libavformat/url.h, unchanged since 2011).
// FFmpeg's own RTMP client switches its connection to non-blocking for the
// read it does after every write by setting AVIO_FLAG_NONBLOCK in `flags`;
// avio.h has no call for that, so the relay does the same through this, after
// checking that the AVIOContext's opaque really is a URLContext for our URL
// (Open() returns Unsupported otherwise, and FFmpeg's client is used).
struct UrlContextHead {
	const AVClass *avClass;
	const void *prot;
	void *privData;
	char *filename;
	int flags;
};

std::string AvError(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}

int64_t NowUs()
{
	return av_gettime_relative();
}

uint32_t Be32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// HMAC-SHA256 of `data` minus the 32 bytes at `gap` (FFmpeg's
// ff_rtmp_calc_digest).
bool Digest(const uint8_t *data, size_t size, size_t gap, const uint8_t *key, size_t keyBytes, uint8_t out[32])
{
	AVHMAC *hmac = av_hmac_alloc(AV_HMAC_SHA256);
	if (!hmac)
		return false;
	av_hmac_init(hmac, key, static_cast<unsigned>(keyBytes));
	av_hmac_update(hmac, data, static_cast<unsigned>(gap));
	av_hmac_update(hmac, data + gap + 32, static_cast<unsigned>(size - gap - 32));
	av_hmac_final(hmac, out, 32);
	av_hmac_free(hmac);
	return true;
}

// C0 and C1 the way FFmpeg's client builds them: version 3, uptime 0, a
// player version, random bytes, and a digest at the position bytes 8-11
// select.
bool MakeC0C1(uint8_t out[1 + kHandshakeBytes])
{
	AVLFG rnd;
	av_lfg_init(&rnd, av_get_random_seed());
	out[0] = 3;
	uint8_t *c1 = out + 1;
	std::memset(c1, 0, 4);
	std::memcpy(c1 + 4, kClientVersion, 4);
	for (size_t i = 8; i < kHandshakeBytes; ++i)
		c1[i] = static_cast<uint8_t>(av_lfg_get(&rnd) >> 24);
	const size_t pos = (size_t(c1[8]) + c1[9] + c1[10] + c1[11]) % 728 + 12;
	return Digest(c1, kHandshakeBytes, pos, kPlayerKey, sizeof(kPlayerKey), c1 + pos);
}

std::string Clip(std::string text)
{
	if (text.size() > 160)
		text = text.substr(0, 157) + "...";
	return text;
}

} // namespace

struct RtmpPublisher::Impl {
	enum class Stage { Closed, Connecting, CreatingStream, Publishing, Live };

	AVIOContext *net = nullptr;
	UrlContextHead *url = nullptr;
	AVIOContext *muxIo = nullptr;
	Stage stage = Stage::Closed;
	std::string key;
	uint32_t streamId = 0;
	double nextTxn = 6;

	ChunkReader reader;
	FlvToRtmp flv;
	std::vector<uint8_t> out; // Complete messages waiting to be written.
	bool muxFailed = false;

	uint64_t bytesSent = 0;
	uint64_t bytesIn = 0;
	uint64_t ackedIn = 0;
	uint32_t serverAckWindow = 0;
	int64_t lastReadUs = 0;
	int readErrorCode = 0; // The last failed read's, during the setup.

	// Set by a message that ends the stream.
	bool ended = false;
	bool serverRefused = false;
	std::string endReason;

	~Impl() { CloseNow(); }

	static int OnMuxerBytes(void *opaque,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
				const uint8_t *buf,
#else
				uint8_t *buf,
#endif
				int size)
	{
		auto *self = static_cast<Impl *>(opaque);
		if (size > 0 && !self->flv.Feed(buf, static_cast<size_t>(size), self->out)) {
			self->muxFailed = true;
			return AVERROR(EIO);
		}
		return size;
	}

	void SetNonblocking(bool on)
	{
		if (on)
			url->flags |= AVIO_FLAG_NONBLOCK;
		else
			url->flags &= ~AVIO_FLAG_NONBLOCK;
	}

	bool Send(const uint8_t *data, size_t size, std::string &error)
	{
		if (size == 0)
			return true;
		avio_write(net, data, static_cast<int>(size));
		avio_flush(net);
		if (net->error < 0) {
			error = AvError(net->error);
			return false;
		}
		bytesSent += size;
		return true;
	}

	bool SendPending(std::string &error)
	{
		if (out.empty())
			return true;
		const bool ok = Send(out.data(), out.size(), error);
		out.clear();
		// A catch-up burst can make it large: don't keep that.
		if (out.capacity() > (4u << 20))
			std::vector<uint8_t>().swap(out);
		return ok;
	}

	// name(txn, null, strings..., number?)
	void Command(const char *name, double txn, std::initializer_list<std::string> strings, const double *number,
		     uint32_t csid, uint32_t msid)
	{
		std::vector<uint8_t> body;
		AmfWriter w(body);
		w.String(name);
		w.Number(txn);
		w.Null();
		for (const std::string &text : strings)
			w.String(text);
		if (number)
			w.Number(*number);
		AppendMessage(out, csid, kCommandAmf0, msid, 0, body.data(), body.size(), kOutChunkSize);
	}

	void SendConnect(const ServerUrl &server)
	{
		std::vector<uint8_t> body;
		AmfWriter w(body);
		w.String("connect");
		w.Number(kTxnConnect);
		w.ObjectStart();
		w.Field("app");
		w.String(server.app);
		w.Field("type");
		w.String("nonprivate");
		w.Field("flashVer");
		w.String(kFlashVer);
		w.Field("tcUrl");
		w.String(server.tcUrl);
		w.ObjectEnd();
		AppendMessage(out, kCsidCommand, kCommandAmf0, 0, 0, body.data(), body.size(), kOutChunkSize);
	}

	void End(bool refused, const std::string &reason)
	{
		if (ended)
			return;
		ended = true;
		serverRefused = refused;
		endReason = Clip(Redact(reason, key));
	}

	static std::string Status(const std::vector<AmfValue> &args, std::string *level)
	{
		for (const AmfValue &arg : args) {
			if (arg.kind != AmfValue::Kind::Object)
				continue;
			if (level)
				*level = arg.Field("level");
			std::string text = arg.Field("code");
			const std::string description = arg.Field("description");
			if (!description.empty() && description != text)
				text += text.empty() ? description : " (" + description + ")";
			return text;
		}
		return {};
	}

	void OnCommand(const rtmp::Command &cmd)
	{
		const auto txn = cmd.transaction;
		if (cmd.name == "_result") {
			if (txn == kTxnConnect && stage == Stage::Connecting) {
				Command("releaseStream", kTxnReleaseStream, {key}, nullptr, kCsidCommand, 0);
				Command("FCPublish", kTxnFcPublish, {key}, nullptr, kCsidCommand, 0);
				AppendWindowAckSize(out, kOurAckWindow);
				Command("createStream", kTxnCreateStream, {}, nullptr, kCsidCommand, 0);
				stage = Stage::CreatingStream;
			} else if (txn == kTxnCreateStream && stage == Stage::CreatingStream) {
				double id = -1;
				for (const AmfValue &arg : cmd.args) {
					if (arg.kind == AmfValue::Kind::Number) {
						id = arg.number;
						break;
					}
				}
				if (id < 0 || id > 0x7FFFFFFF) {
					End(false, "the server's reply to createStream had no stream");
					return;
				}
				streamId = static_cast<uint32_t>(id);
				flv.SetStreamId(streamId);
				Command("publish", kTxnPublish, {key, "live"}, nullptr, kCsidStream, streamId);
				stage = Stage::Publishing;
			}
			return;
		}
		if (cmd.name == "_error") {
			// releaseStream and FCPublish fail on some servers; harmless.
			if (txn == kTxnReleaseStream || txn == kTxnFcPublish)
				return;
			const std::string why = Status(cmd.args, nullptr);
			End(true, "the server refused the stream" + (why.empty() ? std::string() : ": " + why));
			return;
		}
		if (cmd.name == "onStatus") {
			std::string level;
			const std::string why = Status(cmd.args, &level);
			if (level == "error") {
				End(true, "the server ended the stream" + (why.empty() ? std::string() : ": " + why));
				return;
			}
			if (stage == Stage::Publishing && why.rfind("NetStream.Publish.Start", 0) == 0)
				stage = Stage::Live;
			return;
		}
		if (cmd.name == "onBWDone") {
			// FFmpeg and librtmp answer with _checkbw.
			Command("_checkbw", nextTxn++, {}, nullptr, kCsidCommand, 0);
			return;
		}
		if (cmd.name == "close" || cmd.name == "onDisconnect")
			End(true, "the server closed the stream");
	}

	// Handles every complete message read so far; queues the answers.
	bool HandleMessages(std::string &error)
	{
		Message msg;
		while (reader.Next(msg)) {
			switch (msg.type) {
			case kUserControl:
				if (msg.payload.size() >= 6 &&
				    ((uint32_t(msg.payload[0]) << 8) | msg.payload[1]) == kPingRequest)
					AppendPingResponse(out, msg.timestamp, msg.payload.data() + 2,
							   msg.payload.size() - 2, kOutChunkSize);
				break;
			case kWindowAckSize:
				if (msg.payload.size() >= 4)
					serverAckWindow = Be32(msg.payload.data());
				break;
			case kCommandAmf0:
			case kCommandAmf3: {
				rtmp::Command cmd;
				if (ParseCommand(msg.type, msg.payload.data(), msg.payload.size(), cmd))
					OnCommand(cmd);
				break;
			}
			default: // Chunk size, abort (the reader's), acks, bandwidth, media.
				break;
			}
			if (ended)
				break;
		}
		if (reader.Failed()) {
			error = "the server sent something unreadable";
			return false;
		}
		if (serverAckWindow > 0 && bytesIn - ackedIn >= serverAckWindow) {
			AppendAcknowledgement(out, static_cast<uint32_t>(bytesIn));
			ackedIn = bytesIn;
		}
		if (ended) {
			error = endReason;
			return false;
		}
		return true;
	}

	// Reads what has arrived, without waiting.
	bool ReadAvailable(std::string &error)
	{
		lastReadUs = NowUs();
		uint8_t buffer[4096];
		SetNonblocking(true);
		int ret = 0;
		for (int i = 0; i < 64; ++i) {
			ret = avio_read_partial(net, buffer, sizeof(buffer));
			if (ret <= 0)
				break;
			reader.Feed(buffer, static_cast<size_t>(ret));
			bytesIn += static_cast<uint64_t>(ret);
		}
		SetNonblocking(false);
		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			error = ret == AVERROR_EOF ? "the server closed the connection" : AvError(ret);
			// What it said before closing explains it better.
			std::string said;
			if (!HandleMessages(said) && ended)
				error = said;
			return false;
		}
		return HandleMessages(error) && SendPending(error);
	}

	// Reads until at least one byte arrived (blocking; rw_timeout applies).
	bool ReadSome(std::string &error)
	{
		uint8_t buffer[4096];
		const int ret = avio_read_partial(net, buffer, sizeof(buffer));
		if (ret == 0 || ret == AVERROR(EAGAIN))
			return true;
		if (ret < 0) {
			readErrorCode = ret;
			error = ret == AVERROR_EOF ? ClosedText() : AvError(ret);
			return false;
		}
		reader.Feed(buffer, static_cast<size_t>(ret));
		bytesIn += static_cast<uint64_t>(ret);
		return true;
	}

	bool ReadExactly(uint8_t *dst, size_t size, int64_t deadlineUs, std::string &error)
	{
		size_t got = 0;
		while (got < size) {
			if (NowUs() > deadlineUs) {
				error = "the server didn't answer the handshake";
				return false;
			}
			const int ret = avio_read_partial(net, dst + got, static_cast<int>(size - got));
			if (ret == 0 || ret == AVERROR(EAGAIN))
				continue;
			if (ret < 0) {
				readErrorCode = ret;
				error = ret == AVERROR_EOF ? "the server closed the connection during the handshake"
							   : AvError(ret);
				return false;
			}
			got += static_cast<size_t>(ret);
		}
		bytesIn += size;
		return true;
	}

	// Where the server hung up says why: right after publish it is almost
	// always the stream key (Twitch, YouTube and Kick just close).
	std::string ClosedText() const
	{
		switch (stage) {
		case Stage::Connecting:
			return "the server closed the connection after connect";
		case Stage::CreatingStream:
			return "the server closed the connection before the stream was created";
		case Stage::Publishing:
			return "the server closed the connection when the stream started (is the stream key right?)";
		default:
			return "the server closed the connection";
		}
	}

	void CloseNow()
	{
		if (net)
			avio_closep(&net);
		url = nullptr;
		if (muxIo) {
			av_freep(&muxIo->buffer);
			avio_context_free(&muxIo);
		}
		stage = Stage::Closed;
	}
};

RtmpPublisher::RtmpPublisher() : impl_(std::make_unique<Impl>()) {}

RtmpPublisher::~RtmpPublisher() = default;

bool RtmpPublisher::Handles(const std::string &server)
{
	ServerUrl parsed;
	return ParseServerUrl(server, parsed);
}

RtmpPublisher::OpenResult RtmpPublisher::Open(const std::string &server, const std::string &key,
					      const AVIOInterruptCB *interrupt, std::string &error)
{
	impl_ = std::make_unique<Impl>(); // Closes a previous connection.
	Impl &d = *impl_;
	ServerUrl parsed;
	if (!ParseServerUrl(server, parsed)) {
		error = "not an rtmp:// or rtmps:// address";
		return OpenResult::Unsupported;
	}
	d.key = key;

	// The connection: FFmpeg's tcp:// or tls:// (certificate and host checks
	// on, as for FFmpeg's rtmps://).
	const bool v6 = parsed.host.find(':') != std::string::npos;
	const std::string transport = std::string(parsed.tls ? "tls://" : "tcp://") + (v6 ? "[" : "") + parsed.host +
				      (v6 ? "]" : "") + ":" + std::to_string(parsed.port);
	AVDictionary *opts = nullptr;
	av_dict_set(&opts, "rw_timeout", kRwTimeoutUs, 0);
	av_dict_set(&opts, "tcp_nodelay", "1", 0);
	int ret = avio_open2(&d.net, transport.c_str(), AVIO_FLAG_READ_WRITE, interrupt, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		error = AvError(ret);
		return OpenResult::Failed;
	}
	auto *head = static_cast<UrlContextHead *>(d.net->opaque);
	const bool looksRight = head && head->avClass && head->avClass->class_name &&
				std::strcmp(head->avClass->class_name, "URLContext") == 0 && head->filename &&
				transport == head->filename &&
				(head->flags & AVIO_FLAG_READ_WRITE) == AVIO_FLAG_READ_WRITE &&
				(head->flags & AVIO_FLAG_NONBLOCK) == 0 && d.net->write_flag;
	if (!looksRight) {
		d.CloseNow();
		error = "this FFmpeg's connection can't be read without waiting";
		return OpenResult::Unsupported;
	}
	d.url = head;

	const int64_t deadline = NowUs() + kSetupTimeoutUs;
	uint8_t c0c1[1 + kHandshakeBytes];
	if (!MakeC0C1(c0c1)) {
		d.CloseNow();
		error = "out of memory";
		return OpenResult::Failed;
	}
	if (!d.Send(c0c1, sizeof(c0c1), error)) {
		d.CloseNow();
		return OpenResult::Failed;
	}
	// S0, S1, S2. C2 echoes S1 (what FFmpeg's client sends a server it
	// publishes to).
	// A stop (the interrupt) isn't a protocol problem, nor a server that
	// hangs up or goes quiet right after publish (the stream key).
	const auto readFailure = [&d] {
		const bool notOurs = d.readErrorCode == AVERROR_EXIT || d.stage == Impl::Stage::Publishing;
		d.CloseNow();
		return notOurs ? OpenResult::Failed : OpenResult::ProtocolProblem;
	};
	std::vector<uint8_t> s0s1s2(1 + 2 * kHandshakeBytes);
	if (!d.ReadExactly(s0s1s2.data(), s0s1s2.size(), deadline, error))
		return readFailure();
	d.out.assign(s0s1s2.begin() + 1, s0s1s2.begin() + 1 + static_cast<std::ptrdiff_t>(kHandshakeBytes));
	AppendSetChunkSize(d.out, kOutChunkSize);
	d.SendConnect(parsed);
	d.stage = Impl::Stage::Connecting;
	if (!d.SendPending(error)) {
		d.CloseNow();
		return OpenResult::Failed;
	}

	while (d.stage != Impl::Stage::Live) {
		if (NowUs() > deadline) {
			error = "the server didn't start the stream";
			const bool keyStage = d.stage == Impl::Stage::Publishing;
			d.CloseNow();
			return keyStage ? OpenResult::Failed : OpenResult::ProtocolProblem;
		}
		std::string readError;
		const bool readOk = d.ReadSome(readError);
		std::string handleError;
		const bool handled = d.HandleMessages(handleError) && d.SendPending(handleError);
		if (!handled) {
			error = handleError;
			const bool refused = d.ended && d.serverRefused;
			d.CloseNow();
			return refused ? OpenResult::Failed : OpenResult::ProtocolProblem;
		}
		if (!readOk) {
			error = readError;
			return readFailure();
		}
	}

	d.muxIo = avio_alloc_context(static_cast<unsigned char *>(av_malloc(kMuxerIoBytes)), kMuxerIoBytes, 1, &d,
				     nullptr, &Impl::OnMuxerBytes, nullptr);
	if (!d.muxIo || !d.muxIo->buffer) {
		d.CloseNow();
		error = "out of memory";
		return OpenResult::Failed;
	}
	d.lastReadUs = NowUs();
	return OpenResult::Ok;
}

AVIOContext *RtmpPublisher::MuxerIo()
{
	return impl_->muxIo;
}

bool RtmpPublisher::Flush(std::string &error)
{
	Impl &d = *impl_;
	if (!d.net || !d.muxIo) {
		error = "not connected";
		return false;
	}
	avio_flush(d.muxIo);
	if (d.muxFailed) {
		error = "the muxer's output couldn't be read";
		return false;
	}
	if (!d.SendPending(error))
		return false;
	if (NowUs() - d.lastReadUs >= kReadEveryUs)
		return d.ReadAvailable(error);
	return true;
}

bool RtmpPublisher::ServiceDue() const
{
	return impl_->net && impl_->stage == Impl::Stage::Live && NowUs() - impl_->lastReadUs >= kIdleReadEveryUs;
}

void RtmpPublisher::Close(bool unpublish)
{
	Impl &d = *impl_;
	if (d.net && unpublish && d.stage == Impl::Stage::Live && !d.ended) {
		std::string ignored;
		if (d.muxIo)
			avio_flush(d.muxIo);
		const double id = d.streamId;
		d.Command("FCUnpublish", d.nextTxn++, {d.key}, nullptr, kCsidCommand, 0);
		d.Command("deleteStream", d.nextTxn++, {}, &id, kCsidCommand, 0);
		d.SendPending(ignored);
	}
	d.CloseNow();
}

uint64_t RtmpPublisher::BytesSent() const
{
	return impl_->bytesSent;
}

} // namespace trigglow::relay
