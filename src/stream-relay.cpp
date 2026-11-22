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

#include "stream-relay.hpp"

#include "logging.hpp"
#include "packet-delay-core.hpp"
#include "spill-file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef TRIGGLOW_HAVE_STREAM_RELAY
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}
#endif

namespace trigglow::relay {

std::string HostOf(const std::string &url)
{
	size_t start = url.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t end = url.find_first_of(":/?", start);
	return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

#ifndef TRIGGLOW_HAVE_STREAM_RELAY

bool RelayAvailable()
{
	return false;
}

struct StreamRelay::Impl {};
StreamRelay::StreamRelay() : impl_(std::make_unique<Impl>()) {}
StreamRelay::~StreamRelay() = default;
bool StreamRelay::Start(const std::vector<Destination> &, const RelayOptions &, std::string &, std::string &,
			std::string &errorOut)
{
	errorOut = "stream mode is not available on this system yet";
	return false;
}
void StreamRelay::SetDelay(uint32_t) {}
void StreamRelay::SetKeepSeconds(uint32_t) {}
void StreamRelay::Dump() {}
uint32_t StreamRelay::Delay() const
{
	return 0;
}
void StreamRelay::Stop() {}
bool StreamRelay::Running() const
{
	return false;
}
bool StreamRelay::Finished() const
{
	return true;
}
RelayStatus StreamRelay::GetStatus() const
{
	return {};
}
std::vector<std::pair<uint32_t, uint32_t>> StreamRelay::VideoSizes(bool &allH264) const
{
	allH264 = false;
	return {};
}
bool StreamRelay::SetLoadingClip(std::vector<LoadingVideoTrack>, std::string &why)
{
	why = "stream mode is not available on this system yet";
	return false;
}
bool StreamRelay::HasLoadingClip() const
{
	return false;
}
void StreamRelay::SetEventCallback(EventCallback) {}

#else

namespace {

constexpr const char *kComponent = "stream-relay";
constexpr int64_t kSecondNs = 1000000000LL;
// A destination that falls this far behind its delay (a slow upload) jumps to
// the delay again at the next keyframe instead of letting the delay grow.
constexpr int64_t kMaxLagNs = 4 * kSecondNs;
// Spill: the newest seconds stay in RAM (live and short delays never touch the
// disk, and a lowered delay splices into RAM): this plus two keyframe
// intervals.
constexpr int64_t kRamTailNs = 4 * kSecondNs;
// What each destination sends next is read back this far ahead.
constexpr int64_t kPrefetchNs = 3 * kSecondNs;
// Disk the spill always leaves free on the temp folder's volume.
constexpr uint64_t kDiskReserveBytes = 1ULL << 30;
// Below this much room for it, the history stays in RAM.
constexpr uint64_t kMinSpillBytes = 64ULL << 20;

int64_t NowNs()
{
	return static_cast<int64_t>(av_gettime_relative()) * 1000;
}

std::string AvError(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}

// A free TCP port on 127.0.0.1. FFmpeg binds it right after; another process
// taking it in between only makes Start() fail, never stream elsewhere.
int PickLoopbackPort()
{
#ifdef _WIN32
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return 0;
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	int port = 0;
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
		int len = sizeof(addr);
		if (getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
			port = ntohs(addr.sin_port);
	}
	closesocket(s);
	return port;
#else
	const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0)
		return 0;
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	int port = 0;
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
		socklen_t len = sizeof(addr);
		if (getsockname(s, reinterpret_cast<sockaddr *>(&addr), &len) == 0)
			port = ntohs(addr.sin_port);
	}
	close(s);
	return port;
#endif
}

// True once this process has a socket listening on 127.0.0.1:<port>: the
// relay's FFmpeg listener is up and OBS can connect. Read from the system's TCP
// table, so nothing connects to it (FFmpeg accepts a single client).
bool LoopbackListening(int port)
{
#ifdef _WIN32
	std::vector<unsigned char> buffer(16 * 1024);
	for (int attempt = 0; attempt < 3; ++attempt) {
		ULONG size = static_cast<ULONG>(buffer.size());
		const DWORD ret =
			GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
		if (ret == ERROR_INSUFFICIENT_BUFFER) {
			buffer.resize(size + 4096);
			continue;
		}
		if (ret != NO_ERROR)
			return false;
		const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.data());
		const DWORD self = GetCurrentProcessId();
		for (DWORD i = 0; i < table->dwNumEntries; ++i) {
			const MIB_TCPROW_OWNER_PID &row = table->table[i];
			// The LISTENER table holds listening sockets only; their dwState
			// isn't reliable (0 on recent Windows 11 builds), so not checked.
			if (row.dwOwningPid == self && ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
			    row.dwLocalAddr == htonl(INADDR_LOOPBACK))
				return true;
		}
		return false;
	}
	return false;
#else
	(void)port;
	return false;
#endif
}

// "rtmps://host:443/app/sub" -> "app/sub" (FFmpeg's rtmp_app).
std::string AppOf(const std::string &server)
{
	size_t start = server.find("://");
	start = start == std::string::npos ? 0 : start + 3;
	size_t slash = server.find('/', start);
	if (slash == std::string::npos)
		return {};
	std::string app = server.substr(slash + 1);
	while (!app.empty() && app.back() == '/')
		app.pop_back();
	return app;
}

struct StreamInfo {
	MediaKind kind = MediaKind::Other;
	AVCodecParameters *par = nullptr;
	AVRational timeBase{1, 1000};
};

// A packet's record in the history (PayloadStore): what av_read_frame gave,
// so the destination gets the very same bytes, flags and side data.
//   u32 flags, u32 side data count, { u32 type, u32 size, bytes } x count,
//   payload
constexpr size_t kRecordHeaderBytes = 8;
constexpr size_t kSideHeaderBytes = 8;

void PutU32(uint8_t *at, uint32_t value)
{
	std::memcpy(at, &value, sizeof(value));
}

uint32_t GetU32(const uint8_t *at)
{
	uint32_t value = 0;
	std::memcpy(&value, at, sizeof(value));
	return value;
}

// Builds the record's pieces for `pkt` (`meta` holds the headers; the spans
// point into it and into the packet, which must outlive the Push()).
void RecordParts(const AVPacket *pkt, std::vector<uint8_t> &meta, std::vector<PayloadStore::Span> &spans)
{
	const int sideCount = pkt->side_data_elems > 0 ? pkt->side_data_elems : 0;
	meta.resize(kRecordHeaderBytes + static_cast<size_t>(sideCount) * kSideHeaderBytes);
	PutU32(meta.data(), static_cast<uint32_t>(pkt->flags));
	PutU32(meta.data() + 4, static_cast<uint32_t>(sideCount));
	spans.clear();
	spans.push_back({meta.data(), kRecordHeaderBytes});
	for (int i = 0; i < sideCount; ++i) {
		const AVPacketSideData &side = pkt->side_data[i];
		uint8_t *header = meta.data() + kRecordHeaderBytes + static_cast<size_t>(i) * kSideHeaderBytes;
		PutU32(header, static_cast<uint32_t>(side.type));
		PutU32(header + 4, static_cast<uint32_t>(side.size));
		spans.push_back({header, kSideHeaderBytes});
		spans.push_back({side.data, side.size});
	}
	spans.push_back({pkt->data, static_cast<size_t>(pkt->size)});
}

// Rebuilds the packet from its record into `pk` (blank). False when the
// record is malformed.
bool PacketFromRecord(const uint8_t *record, size_t bytes, AVPacket *pk)
{
	if (bytes < kRecordHeaderBytes)
		return false;
	const uint32_t flags = GetU32(record);
	const uint32_t sideCount = GetU32(record + 4);
	size_t at = kRecordHeaderBytes;
	struct Side {
		uint32_t type;
		uint32_t size;
		const uint8_t *data;
	};
	std::vector<Side> sides;
	sides.reserve(sideCount);
	for (uint32_t i = 0; i < sideCount; ++i) {
		if (bytes - at < kSideHeaderBytes)
			return false;
		const Side side{GetU32(record + at), GetU32(record + at + 4), record + at + kSideHeaderBytes};
		at += kSideHeaderBytes;
		if (bytes - at < side.size)
			return false;
		at += side.size;
		sides.push_back(side);
	}
	const size_t payload = bytes - at;
	if (av_new_packet(pk, static_cast<int>(payload)) < 0)
		return false;
	if (payload > 0)
		std::memcpy(pk->data, record + at, payload);
	pk->flags = static_cast<int>(flags);
	for (const Side &side : sides) {
		uint8_t *dst = av_packet_new_side_data(pk, static_cast<AVPacketSideDataType>(side.type), side.size);
		if (!dst) {
			av_packet_unref(pk);
			return false;
		}
		if (side.size > 0)
			std::memcpy(dst, side.data, side.size);
	}
	return true;
}

// A due packet, its record copied into the sink's scratch buffer.
struct Outgoing {
	size_t recordAt = 0;
	size_t recordBytes = 0;
	int stream = 0;
	int64_t ptsMs = 0;
	int64_t dtsMs = 0;
	int64_t arrivalNs = 0;
	// v1.1.0 loading clip: the bytes are the payload itself (not a record),
	// and the codec header to send before it when the source changed.
	bool raw = false;
	bool keyframe = false;
	size_t headerAt = 0;
	size_t headerBytes = 0;
};

// The NAL units of an Annex B buffer (00 00 01 / 00 00 00 01 start codes).
std::vector<std::pair<const uint8_t *, size_t>> AnnexBUnits(const uint8_t *data, size_t size)
{
	std::vector<std::pair<const uint8_t *, size_t>> units;
	size_t i = 0;
	auto startCode = [&](size_t at) -> size_t {
		if (at + 3 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1)
			return 3;
		if (at + 4 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 0 && data[at + 3] == 1)
			return 4;
		return 0;
	};
	size_t begin = SIZE_MAX;
	while (i < size) {
		const size_t sc = startCode(i);
		if (sc) {
			if (begin != SIZE_MAX && i > begin)
				units.emplace_back(data + begin, i - begin);
			i += sc;
			begin = i;
			continue;
		}
		++i;
	}
	if (begin != SIZE_MAX && size > begin)
		units.emplace_back(data + begin, size - begin);
	// Trailing zero bytes belong to the next start code, not the unit.
	for (auto &u : units) {
		while (u.second > 1 && u.first[u.second - 1] == 0)
			--u.second;
	}
	return units;
}

// Annex B -> length-prefixed (what FLV carries), 4-byte lengths.
std::vector<uint8_t> AnnexBToAvcc(const std::vector<uint8_t> &in)
{
	std::vector<uint8_t> out;
	out.reserve(in.size() + 16);
	for (const auto &[unit, size] : AnnexBUnits(in.data(), in.size())) {
		out.push_back(static_cast<uint8_t>(size >> 24));
		out.push_back(static_cast<uint8_t>(size >> 16));
		out.push_back(static_cast<uint8_t>(size >> 8));
		out.push_back(static_cast<uint8_t>(size));
		out.insert(out.end(), unit, unit + size);
	}
	return out;
}

// The avcC record (ISO 14496-15) from Annex B SPS/PPS; empty without them.
std::vector<uint8_t> AvccHeader(const std::vector<uint8_t> &annexB)
{
	std::vector<std::pair<const uint8_t *, size_t>> sps;
	std::vector<std::pair<const uint8_t *, size_t>> pps;
	for (const auto &unit : AnnexBUnits(annexB.data(), annexB.size())) {
		const int type = unit.first[0] & 0x1F;
		if (type == 7 && unit.second >= 4)
			sps.push_back(unit);
		else if (type == 8)
			pps.push_back(unit);
	}
	if (sps.empty() || pps.empty())
		return {};
	std::vector<uint8_t> out{1,
				 sps[0].first[1],
				 sps[0].first[2],
				 sps[0].first[3],
				 0xFF,
				 static_cast<uint8_t>(0xE0 | (sps.size() & 0x1F))};
	auto put = [&out](const std::pair<const uint8_t *, size_t> &unit) {
		out.push_back(static_cast<uint8_t>(unit.second >> 8));
		out.push_back(static_cast<uint8_t>(unit.second));
		out.insert(out.end(), unit.first, unit.first + unit.second);
	};
	for (const auto &unit : sps)
		put(unit);
	out.push_back(static_cast<uint8_t>(pps.size()));
	for (const auto &unit : pps)
		put(unit);
	return out;
}

// The SPS and PPS of an avcC record as length-prefixed NAL units (4-byte
// lengths), to put in front of a keyframe; empty when it isn't one.
std::vector<uint8_t> AvccParamNals(const uint8_t *avcc, size_t size)
{
	std::vector<uint8_t> out;
	if (!avcc || size < 7 || avcc[0] != 1)
		return out;
	size_t at = 5;
	auto take = [&](int count) {
		for (int i = 0; i < count; ++i) {
			if (at + 2 > size)
				return false;
			const size_t len = (static_cast<size_t>(avcc[at]) << 8) | avcc[at + 1];
			at += 2;
			if (at + len > size)
				return false;
			out.push_back(static_cast<uint8_t>(len >> 24));
			out.push_back(static_cast<uint8_t>(len >> 16));
			out.push_back(static_cast<uint8_t>(len >> 8));
			out.push_back(static_cast<uint8_t>(len));
			out.insert(out.end(), avcc + at, avcc + at + len);
			at += len;
		}
		return true;
	};
	if (!take(avcc[5] & 0x1F) || at >= size) {
		out.clear();
		return out;
	}
	const int pps = avcc[at++];
	if (!take(pps))
		out.clear();
	return out;
}

// Silence in AAC for an audio stream like `par`: one encoded frame repeated
// (silence encodes the same every frame), enough of them that the loop is a
// whole number of milliseconds. False when FFmpeg's AAC encoder can't.
bool SilentAac(const AVCodecParameters *par, std::vector<uint8_t> &frameOut, std::vector<uint8_t> &headerOut,
	       int &frameSamples)
{
	if (!par || par->codec_id != AV_CODEC_ID_AAC || par->sample_rate <= 0)
		return false;
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec)
		return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;
	ctx->sample_rate = par->sample_rate;
	if (av_channel_layout_copy(&ctx->ch_layout, &par->ch_layout) < 0 || ctx->ch_layout.nb_channels <= 0)
		av_channel_layout_default(&ctx->ch_layout, 2);
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	ctx->bit_rate = 64000;
	ctx->time_base = AVRational{1, par->sample_rate};
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	bool ok = false;
	AVFrame *frame = av_frame_alloc();
	AVPacket *pkt = av_packet_alloc();
	if (frame && pkt && avcodec_open2(ctx, codec, nullptr) >= 0) {
		frame->nb_samples = ctx->frame_size > 0 ? ctx->frame_size : 1024;
		frame->format = ctx->sample_fmt;
		av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
		frame->sample_rate = ctx->sample_rate;
		if (av_frame_get_buffer(frame, 0) >= 0) {
			// Past the encoder's priming: the frames after are steady.
			for (int i = 0; i < 8 && !ok; ++i) {
				av_frame_make_writable(frame);
				for (int c = 0; c < frame->ch_layout.nb_channels; ++c)
					std::memset(frame->extended_data[c], 0,
						    static_cast<size_t>(frame->nb_samples) * sizeof(float));
				frame->pts = static_cast<int64_t>(i) * frame->nb_samples;
				if (avcodec_send_frame(ctx, frame) < 0)
					break;
				while (avcodec_receive_packet(ctx, pkt) >= 0) {
					if (i >= 5 && pkt->size > 0 && !ok) {
						frameOut.assign(pkt->data, pkt->data + pkt->size);
						ok = true;
					}
					av_packet_unref(pkt);
				}
			}
			frameSamples = frame->nb_samples;
		}
		if (ok && ctx->extradata && ctx->extradata_size > 0)
			headerOut.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
	}
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&ctx);
	return ok;
}

// The loading clip in the history's terms (packet-delay-core.hpp Filler),
// with the bytes and codec header of each track.
struct LoadingStore {
	Filler filler;
	std::vector<std::vector<uint8_t>> payloads; // Packet::recordOffset indexes these.
	std::vector<std::vector<uint8_t>> headers;  // One per filler track.
};

} // namespace

bool RelayAvailable()
{
	return true;
}

struct StreamRelay::Impl {
	struct Sink {
		Destination dest;
		SinkScheduler sched;
		std::thread thread;
		std::atomic<bool> abort{false};
		SinkStatus status;
		uint64_t appliedGen = 0;
		bool finished = false;
		int64_t statBytes = 0;
		int64_t statNs = 0;
	};

	mutable std::mutex m;
	std::condition_variable cv;
	PacketHistory history;
	std::vector<StreamInfo> streams;
	bool streamsReady = false;
	std::atomic<bool> stop{false};
	bool running = false;
	bool ingestConnected = false;
	bool ingestEnded = false;
	uint32_t delaySeconds = 0;
	uint64_t delayGen = 1;
	RelayOptions options;
	double keyframeInterval = 0.0;
	int64_t keyframeCheckNs = 0;
	std::vector<std::unique_ptr<Sink>> sinks;
	std::thread ingest;
	// Spill (null: RAM only). The I/O thread is the only one that touches the
	// file; it waits on ioCv.
	std::unique_ptr<SpillFile> spill;
	std::thread io;
	std::condition_variable ioCv;
	SpillState loggedSpill = SpillState::Off;
	std::string spillNote; // Why the spill is off or stopped (status).
	EventCallback callback;
	std::string listenUrl;
	// v1.1.0 loading screen (SetLoadingClip).
	std::shared_ptr<const LoadingStore> loading;

	void Event(const std::string &message)
	{
		EventCallback cb;
		{
			std::lock_guard<std::mutex> lock(m);
			cb = callback;
		}
		if (cb)
			cb(message);
	}

	static int InterruptIngest(void *opaque) { return static_cast<Impl *>(opaque)->stop.load() ? 1 : 0; }

	struct SinkInterruptCtx {
		Impl *impl;
		Sink *sink;
	};

	static int InterruptSink(void *opaque)
	{
		auto *ctx = static_cast<SinkInterruptCtx *>(opaque);
		return (ctx->impl->stop.load() || ctx->sink->abort.load()) ? 1 : 0;
	}

	// See KeepSeconds() (packet-delay-core.hpp).
	uint32_t KeepSecondsLocked() const
	{
		return KeepSeconds(history.Store().Spill() == SpillState::Active, options.keepSeconds);
	}

	uint64_t MinSinkCursorLocked() const
	{
		uint64_t minSeq = UINT64_MAX;
		for (const auto &sink : sinks) {
			if (sink->sched.Started() && !sink->finished)
				minSeq = std::min(minSeq, sink->sched.CursorSeq());
		}
		return minSeq;
	}

	void IngestMain()
	{
		AVFormatContext *ic = avformat_alloc_context();
		if (!ic) {
			FinishIngest("out of memory");
			return;
		}
		ic->interrupt_callback.callback = &Impl::InterruptIngest;
		ic->interrupt_callback.opaque = this;
		ic->probesize = 1 << 20;
		ic->max_analyze_duration = 3 * AV_TIME_BASE;

		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "listen", "1", 0);
		av_dict_set(&opts, "tcp_nodelay", "1", 0);
		int ret = avformat_open_input(&ic, listenUrl.c_str(), nullptr, &opts);
		av_dict_free(&opts);
		if (ret < 0) {
			if (!stop.load())
				TRIGGLOW_LOG_ERROR(kComponent, "OBS's stream never reached the relay: %s",
						   AvError(ret).c_str());
			FinishIngest(stop.load() ? "" : "OBS's stream never reached the relay");
			return;
		}
		ret = avformat_find_stream_info(ic, nullptr);
		if (ret < 0) {
			TRIGGLOW_LOG_ERROR(kComponent, "couldn't read OBS's stream format: %s", AvError(ret).c_str());
			avformat_close_input(&ic);
			FinishIngest("couldn't read OBS's stream format");
			return;
		}

		{
			std::lock_guard<std::mutex> lock(m);
			for (unsigned i = 0; i < ic->nb_streams; ++i) {
				const AVStream *st = ic->streams[i];
				StreamInfo info;
				info.kind = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO   ? MediaKind::Video
					    : st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ? MediaKind::Audio
											     : MediaKind::Other;
				info.par = avcodec_parameters_alloc();
				avcodec_parameters_copy(info.par, st->codecpar);
				info.timeBase = st->time_base;
				streams.push_back(info);
				if (info.kind == MediaKind::Video)
					TRIGGLOW_LOG_INFO(kComponent, "OBS stream: video %s %dx%d (track %u)",
							  avcodec_get_name(st->codecpar->codec_id), st->codecpar->width,
							  st->codecpar->height, i);
				else if (info.kind == MediaKind::Audio)
					TRIGGLOW_LOG_INFO(kComponent, "OBS stream: audio %s %d Hz",
							  avcodec_get_name(st->codecpar->codec_id),
							  st->codecpar->sample_rate);
			}
			streamsReady = true;
			ingestConnected = true;
		}
		cv.notify_all();

		AVPacket *pkt = av_packet_alloc();
		const AVRational ms{1, 1000};
		std::vector<uint8_t> recordMeta;
		std::vector<PayloadStore::Span> recordSpans;
		while (!stop.load()) {
			ret = av_read_frame(ic, pkt);
			if (ret < 0)
				break;
			const int index = pkt->stream_index;
			if (index < 0 || static_cast<size_t>(index) >= streams.size() || index > 255) {
				av_packet_unref(pkt);
				continue;
			}
			const AVStream *st = ic->streams[index];
			const int64_t rawDts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
			const int64_t rawPts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : rawDts;
			if (rawDts == AV_NOPTS_VALUE) {
				av_packet_unref(pkt);
				continue;
			}
			Packet p;
			p.kind = streams[index].kind;
			p.stream = static_cast<uint8_t>(index);
			p.dtsMs = av_rescale_q(rawDts, st->time_base, ms);
			p.ptsMs = av_rescale_q(rawPts, st->time_base, ms);
			p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
			p.arrivalNs = NowNs();
			p.bytes = static_cast<uint32_t>(pkt->size);
			// The bytes are copied into the history's blocks: no FFmpeg
			// allocation is kept per packet.
			RecordParts(pkt, recordMeta, recordSpans);
			bool blockFilled = false;
			{
				std::lock_guard<std::mutex> lock(m);
				const size_t blockBytes = history.Store().BlockBytes();
				const uint64_t endBefore = history.Store().EndOffset() / blockBytes;
				history.Push(p, recordSpans.data(), recordSpans.size());
				blockFilled = spill && history.Store().EndOffset() / blockBytes != endBefore;
				const int64_t now = NowNs();
				if (now - keyframeCheckNs > 2 * kSecondNs) {
					keyframeInterval = history.KeyframeIntervalSeconds();
					keyframeCheckNs = now;
				}
				history.Trim(now, RetentionNs(delaySeconds, KeepSecondsLocked(), keyframeInterval),
					     MinSinkCursorLocked());
			}
			av_packet_unref(pkt);
			cv.notify_all();
			if (blockFilled)
				ioCv.notify_one();
		}
		av_packet_free(&pkt);
		avformat_close_input(&ic);
		TRIGGLOW_LOG_INFO(kComponent, "OBS stopped sending (%s)",
				  ret == AVERROR_EOF ? "end of stream" : AvError(ret).c_str());
		FinishIngest("");
	}

	// The spill's I/O: writes old blocks out, reads back what destinations
	// are about to send, never under the lock while the disk works.
	void IoMain()
	{
		std::vector<PayloadStore::Range> needed;
		std::unique_lock<std::mutex> lock(m);
		while (!stop.load()) {
			const double gop = std::max(keyframeInterval, 2.0);
			const int64_t ramNs = kRamTailNs + static_cast<int64_t>(2.0 * gop * 1e9);
			const uint64_t ramFrom = history.OffsetAtArrival(NowNs() - ramNs);
			needed.clear();
			for (const auto &sink : sinks) {
				if (sink->sched.Started() && !sink->finished)
					needed.push_back(history.AheadRange(sink->sched.CursorSeq(), kPrefetchNs));
			}
			PayloadStore &store = history.Store();
			PayloadStore::IoJob job = store.PlanIo(ramFrom, needed);
			LogSpillLocked();
			if (job.kind == PayloadStore::IoJob::Kind::None) {
				ioCv.wait_for(lock, std::chrono::milliseconds(50));
				continue;
			}
			lock.unlock();
			store.RunIo(job); // Only the backend and the job: no lock needed.
			lock.lock();
			store.FinishIo(job);
			history.DropLost();
			LogSpillLocked();
			cv.notify_all();
		}
	}

	void LogSpillLocked()
	{
		const SpillState state = history.Store().Spill();
		if (state == loggedSpill)
			return;
		loggedSpill = state;
		if (state == SpillState::Fallback) {
			spillNote = history.Store().SpillProblem();
			TRIGGLOW_LOG_WARN(kComponent, "the history stays in RAM from now on: %s (the stream goes on)",
					  spillNote.c_str());
		}
		if (history.Store().LostBefore() > 0)
			TRIGGLOW_LOG_WARN(kComponent, "part of the history couldn't be read back from the disk");
	}

	void FinishIngest(const std::string &error)
	{
		{
			std::lock_guard<std::mutex> lock(m);
			ingestEnded = true;
			ingestConnected = false;
			// Nothing to drain without a delay, or when the streamer said so.
			if (!options.drainOnStop || delaySeconds == 0) {
				for (auto &sink : sinks)
					sink->abort.store(true);
			}
		}
		cv.notify_all();
		if (!error.empty())
			Event(error);
	}

	bool Connect(Sink &sink, AVFormatContext *&oc, std::vector<int> &map, SinkInterruptCtx &ictx,
		     std::string &error)
	{
		const std::string &server = sink.dest.server;
		int ret = avformat_alloc_output_context2(&oc, nullptr, "flv", server.c_str());
		if (ret < 0 || !oc) {
			error = AvError(ret);
			return false;
		}
		oc->interrupt_callback.callback = &Impl::InterruptSink;
		oc->interrupt_callback.opaque = &ictx;
		map.assign(streams.size(), -1);
		bool videoMapped = false;
		for (size_t i = 0; i < streams.size(); ++i) {
			if (streams[i].kind == MediaKind::Other)
				continue;
			// Enhanced Broadcasting sends several video tracks; only the
			// streamer's own service (the platform that asked for them) gets
			// them all. Multistream channels get the first (main) one, as a
			// regular stream.
			if (streams[i].kind == MediaKind::Video && videoMapped && !sink.dest.primary)
				continue;
			if (streams[i].kind == MediaKind::Video)
				videoMapped = true;
			AVStream *st = avformat_new_stream(oc, nullptr);
			if (!st || avcodec_parameters_copy(st->codecpar, streams[i].par) < 0) {
				error = "couldn't set up the stream";
				return false;
			}
			st->codecpar->codec_tag = 0;
			st->time_base = AVRational{1, 1000};
			map[i] = st->index;
		}

		AVDictionary *io = nullptr;
		const std::string app = AppOf(server);
		if (!app.empty())
			av_dict_set(&io, "rtmp_app", app.c_str(), 0);
		av_dict_set(&io, "rtmp_playpath", sink.dest.key.c_str(), 0);
		// v1.1.0 Enhanced Broadcasting: several video tracks (or HEVC/AV1) go
		// as Enhanced RTMP, announced in the connect like OBS does.
		size_t videoTracks = 0;
		bool enhancedCodec = false;
		for (const StreamInfo &info : streams) {
			if (info.kind != MediaKind::Video || !info.par)
				continue;
			++videoTracks;
			enhancedCodec = enhancedCodec || info.par->codec_id == AV_CODEC_ID_HEVC ||
					info.par->codec_id == AV_CODEC_ID_AV1;
		}
		if ((videoTracks > 1 && sink.dest.primary) || enhancedCodec)
			av_dict_set(&io, "rtmp_enhanced_codecs", "hvc1,av01,avc1", 0);
		av_dict_set(&io, "rw_timeout", "15000000", 0);
		// A fixed 64 KB socket buffer caps one TCP connection at about
		// 64 KB / RTT (17 Mbps at 30 ms): Enhanced Broadcasting's tracks
		// together fell behind on a real uplink. OBS's own output grows its
		// buffer with Windows' ideal send backlog; 8 MB is plenty here.
		av_dict_set(&io, "send_buffer_size", "8388608", 0);
		av_dict_set(&io, "tcp_nodelay", "1", 0);
		ret = avio_open2(&oc->pb, server.c_str(), AVIO_FLAG_WRITE, &oc->interrupt_callback, &io);
		av_dict_free(&io);
		if (ret < 0) {
			error = AvError(ret);
			return false;
		}
		AVDictionary *mux = nullptr;
		av_dict_set(&mux, "flvflags", "no_duration_filesize", 0);
		ret = avformat_write_header(oc, &mux);
		av_dict_free(&mux);
		if (ret < 0) {
			error = AvError(ret);
			return false;
		}
		return true;
	}

	static void Disconnect(AVFormatContext *&oc, bool writeTrailer)
	{
		if (!oc)
			return;
		if (writeTrailer && oc->pb)
			av_write_trailer(oc);
		if (oc->pb)
			avio_closep(&oc->pb);
		avformat_free_context(oc);
		oc = nullptr;
	}

	void SinkMain(Sink &sink)
	{
		AVFormatContext *oc = nullptr;
		std::vector<int> map;
		SinkInterruptCtx ictx{this, &sink};
		int backoffSeconds = 2;
		bool gaveUp = false;
		// The destination's name (never the key) and host, so the log tells
		// the destinations apart.
		const std::string serverHost = HostOf(sink.dest.server);
		const std::string host = sink.dest.name.empty() || sink.dest.name == serverHost
						 ? serverHost
						 : sink.dest.name + " (" + serverHost + ")";
		// Records of the packets being sent, copied out of the history under
		// the lock (the history may drop them right after), and the packet
		// rebuilt from each.
		std::vector<uint8_t> scratch;
		std::vector<Outgoing> out;
		AVPacket *pk = av_packet_alloc();

		std::unique_lock<std::mutex> lock(m);
		while (!stop.load() && !sink.abort.load()) {
			if (!streamsReady) {
				if (ingestEnded)
					break;
				cv.wait_for(lock, std::chrono::milliseconds(100));
				continue;
			}
			const int64_t delayNs = static_cast<int64_t>(delaySeconds) * kSecondNs;
			if (sink.appliedGen != delayGen) {
				const SpliceInfo info = sink.sched.SetDelay(delayNs, NowNs(), history);
				sink.appliedGen = delayGen;
				if (info.filling)
					TRIGGLOW_LOG_INFO(
						kComponent,
						"%s: delay now %us (the loading screen shows while it builds up)",
						host.c_str(), delaySeconds);
				else if (info.spliced && info.jumpSeconds != 0.0)
					TRIGGLOW_LOG_INFO(kComponent, "%s: delay now %us (%s %.1f s)", host.c_str(),
							  delaySeconds, info.jumpSeconds > 0 ? "replays" : "skips",
							  info.jumpSeconds > 0 ? info.jumpSeconds : -info.jumpSeconds);
			}
			if (oc && sink.sched.LagNs(NowNs(), history) > kMaxLagNs) {
				sink.sched.SetDelay(delayNs, NowNs(), history);
				++sink.status.resyncs;
				TRIGGLOW_LOG_WARN(kComponent,
						  "%s: the upload fell behind; jumped back to the %us delay",
						  host.c_str(), delaySeconds);
			}

			// With the spill, a packet whose bytes aren't back in RAM yet waits
			// (the I/O thread reads them ahead; this is only the fallback).
			bool waitingForDisk = false;
			std::vector<OutPacket> due = sink.sched.Due(NowNs(), history, 512, [&](const Packet &p) {
				if (history.RecordInRam(p))
					return true;
				waitingForDisk = true;
				return false;
			});
			if (due.empty()) {
				if (ingestEnded && sink.sched.Started() && sink.sched.CaughtUp(history))
					break;
				if (waitingForDisk) {
					ioCv.notify_one();
					cv.wait_for(lock, std::chrono::milliseconds(5));
					continue;
				}
				const int64_t next = sink.sched.NextDueNs(history);
				int64_t waitNs = 100 * 1000000LL;
				if (next >= 0)
					waitNs = std::clamp<int64_t>(next - NowNs(), 1000000LL, waitNs);
				cv.wait_for(lock, std::chrono::nanoseconds(waitNs));
				continue;
			}
			scratch.clear();
			out.clear();
			sink.status.loading = sink.sched.Filling();
			for (const OutPacket &o : due) {
				Outgoing g;
				g.stream = o.packet->stream;
				g.ptsMs = o.ptsMs;
				g.dtsMs = o.dtsMs;
				g.arrivalNs = o.packet->arrivalNs;
				g.recordAt = scratch.size();
				if (o.filler) {
					// A loading-clip packet: its payload as is.
					if (!loading || o.packet->recordOffset >= loading->payloads.size())
						continue;
					const std::vector<uint8_t> &bytes = loading->payloads[o.packet->recordOffset];
					scratch.insert(scratch.end(), bytes.begin(), bytes.end());
					g.recordBytes = bytes.size();
					g.raw = true;
					g.keyframe = o.packet->keyframe;
					g.arrivalNs = NowNs() - delayNs;
				} else {
					if (!history.CopyRecord(*o.packet, scratch))
						continue;
					g.recordBytes = o.packet->recordBytes;
				}
				if (o.newSource && o.packet->kind == MediaKind::Video && o.packet->keyframe) {
					// The source changed (stream <-> loading clip): its SPS and
					// PPS go in front of this keyframe, in band. Not a new FLV
					// sequence header: MediaMTX (Trigglow Multistream) rejects a
					// second one in its first 2 s and ignores it after, and
					// every decoder takes in-band parameters.
					const std::vector<uint8_t> *header = nullptr;
					std::vector<uint8_t> streamHeader;
					if (o.filler && loading) {
						const size_t track =
							static_cast<size_t>(o.filler - loading->filler.tracks.data());
						if (track < loading->headers.size())
							header = &loading->headers[track];
					} else if (static_cast<size_t>(g.stream) < streams.size() &&
						   streams[g.stream].par && streams[g.stream].par->extradata_size > 0) {
						const AVCodecParameters *par = streams[g.stream].par;
						streamHeader.assign(par->extradata,
								    par->extradata + par->extradata_size);
						header = &streamHeader;
					}
					if (header && !header->empty()) {
						const std::vector<uint8_t> nals =
							AvccParamNals(header->data(), header->size());
						g.headerAt = scratch.size();
						g.headerBytes = nals.size();
						scratch.insert(scratch.end(), nals.begin(), nals.end());
					}
				}
				out.push_back(g);
			}
			if (out.empty())
				continue;
			lock.unlock();

			bool ok = true;
			std::string error;
			if (!oc) {
				{
					std::lock_guard<std::mutex> statusLock(m);
					sink.status.state = sink.status.reconnects > 0 ? SinkState::Reconnecting
										       : SinkState::Connecting;
				}
				ok = Connect(sink, oc, map, ictx, error);
				if (ok) {
					TRIGGLOW_LOG_INFO(kComponent, "%s: connected", host.c_str());
					backoffSeconds = 2;
					std::lock_guard<std::mutex> statusLock(m);
					sink.status.state = SinkState::Live;
					sink.status.error.clear();
				}
			}
			for (size_t i = 0; ok && pk && i < out.size(); ++i) {
				const Outgoing &o = out[i];
				const int target = (o.stream >= 0 && static_cast<size_t>(o.stream) < map.size())
							   ? map[o.stream]
							   : -1;
				if (target < 0)
					continue;
				if (o.raw) {
					if (av_new_packet(pk, static_cast<int>(o.recordBytes)) < 0)
						continue;
					std::memcpy(pk->data, scratch.data() + o.recordAt, o.recordBytes);
					pk->flags = o.keyframe ? AV_PKT_FLAG_KEY : 0;
				} else if (!PacketFromRecord(scratch.data() + o.recordAt, o.recordBytes, pk)) {
					continue;
				}
				if (o.headerBytes > 0) {
					// The source's SPS/PPS in front of the keyframe's NAL units.
					const int before = pk->size;
					if (av_grow_packet(pk, static_cast<int>(o.headerBytes)) >= 0) {
						std::memmove(pk->data + o.headerBytes, pk->data,
							     static_cast<size_t>(before));
						std::memcpy(pk->data, scratch.data() + o.headerAt, o.headerBytes);
					}
				}
				const AVRational tb = oc->streams[target]->time_base;
				pk->time_base = streams[o.stream].timeBase;
				pk->stream_index = target;
				pk->pts = av_rescale_q(o.ptsMs, AVRational{1, 1000}, tb);
				pk->dts = av_rescale_q(o.dtsMs, AVRational{1, 1000}, tb);
				pk->duration = 0;
				pk->pos = -1;
				const int ret = av_interleaved_write_frame(oc, pk);
				av_packet_unref(pk);
				if (ret < 0) {
					ok = false;
					error = AvError(ret);
				}
			}
			// A catch-up burst can make the scratch large: don't keep it.
			if (scratch.capacity() > 2 * PayloadStore::kDefaultBlockBytes)
				std::vector<uint8_t>().swap(scratch);

			lock.lock();
			if (ok) {
				const int64_t now = NowNs();
				sink.status.behindSeconds = static_cast<double>(now - out.back().arrivalNs) / 1e9;
				const int64_t written = oc && oc->pb ? avio_tell(oc->pb) : 0;
				sink.status.bytesSent = static_cast<uint64_t>(std::max<int64_t>(written, 0));
				if (sink.statNs == 0) {
					sink.statNs = now;
					sink.statBytes = written;
				} else if (now - sink.statNs >= kSecondNs) {
					sink.status.sentMbps = static_cast<double>(written - sink.statBytes) * 8.0 /
							       (static_cast<double>(now - sink.statNs) / 1e9) / 1e6;
					sink.statNs = now;
					sink.statBytes = written;
				}
				continue;
			}

			if (stop.load() || sink.abort.load())
				break;
			if (ingestEnded) {
				// OBS has stopped: a destination that can't take the last
				// seconds isn't retried, or the stream would never finish.
				sink.status.state = SinkState::Failed;
				sink.status.error = error;
				gaveUp = true;
				lock.unlock();
				TRIGGLOW_LOG_WARN(kComponent, "%s: %s; OBS has stopped, so it isn't retried",
						  host.c_str(), error.c_str());
				Event(sink.dest.name + ": " + error);
				lock.lock();
				break;
			}
			// Failed to connect or to send: reconnect after a back-off, with a
			// fresh timeline, at the delay.
			lock.unlock();
			Disconnect(oc, false);
			TRIGGLOW_LOG_WARN(kComponent, "%s: %s; retrying in %d s", host.c_str(), error.c_str(),
					  backoffSeconds);
			Event(sink.dest.name + ": " + error);
			lock.lock();
			sink.status.state = SinkState::Reconnecting;
			sink.status.error = error;
			++sink.status.reconnects;
			sink.statNs = 0;
			// OBS stopping ends the wait too: one last try for the last seconds.
			cv.wait_for(lock, std::chrono::seconds(backoffSeconds),
				    [&] { return stop.load() || sink.abort.load() || ingestEnded; });
			backoffSeconds = std::min(backoffSeconds * 2, 15);
			sink.sched.ResetTimeline();
			sink.sched.SetDelay(static_cast<int64_t>(delaySeconds) * kSecondNs, NowNs(), history);
		}
		sink.finished = true;
		const bool drained = ingestEnded && !stop.load() && !gaveUp;
		lock.unlock();
		av_packet_free(&pk);
		Disconnect(oc, !gaveUp);
		{
			std::lock_guard<std::mutex> statusLock(m);
			if (sink.status.state != SinkState::Failed)
				sink.status.state = SinkState::Done;
		}
		TRIGGLOW_LOG_INFO(kComponent, "%s: finished%s", host.c_str(), drained ? " (delay sent)" : "");
		cv.notify_all();
	}
};

StreamRelay::StreamRelay() : impl_(std::make_unique<Impl>()) {}

StreamRelay::~StreamRelay()
{
	Stop();
}

bool StreamRelay::Start(const std::vector<Destination> &destinations, const RelayOptions &options,
			std::string &serverOut, std::string &keyOut, std::string &errorOut)
{
	Stop();
	static std::once_flag networkInit;
	std::call_once(networkInit, [] { avformat_network_init(); });

	if (destinations.empty()) {
		errorOut = "no destination";
		return false;
	}
	const int port = PickLoopbackPort();
	if (port <= 0) {
		errorOut = "no free local port for the relay";
		return false;
	}

	Impl &d = *impl_;
	d.stop.store(false);
	d.history.Clear();
	d.loading.reset();
	d.history.Store().SetSpill(nullptr, 0);
	d.spill.reset();
	d.spillNote.clear();
	d.loggedSpill = SpillState::Off;
	// A file a crash (or power loss) left behind goes first, also when this
	// stream keeps its history in memory (buffer_storage).
	const int stale = SpillFile::RemoveStale();
	if (stale > 0)
		TRIGGLOW_LOG_INFO(kComponent, "deleted %d spill file(s) a previous session left", stale);
	if (options.spillToDisk) {
		const size_t blockBytes = d.history.Store().BlockBytes();
		const uint64_t freeBytes = SpillFile::FreeDiskBytes();
		uint64_t room = freeBytes > kDiskReserveBytes ? freeBytes - kDiskReserveBytes : 0;
		if (options.spillLimitBytes > 0)
			room = std::min(room, options.spillLimitBytes);
		std::string why;
		if (room < kMinSpillBytes)
			why = "not enough free disk space";
		else
			d.spill = SpillFile::Create(blockBytes, why);
		if (d.spill) {
			d.history.Store().SetSpill(d.spill.get(), room / blockBytes);
			d.loggedSpill = SpillState::Active;
			TRIGGLOW_LOG_INFO(kComponent,
					  "older history goes to an encrypted temp file (up to %llu MB, %llu MB free)",
					  static_cast<unsigned long long>(room >> 20),
					  static_cast<unsigned long long>(freeBytes >> 20));
		} else {
			d.spillNote = why;
			TRIGGLOW_LOG_WARN(kComponent, "the history stays in RAM: %s", why.c_str());
		}
	}
	d.streams.clear();
	d.streamsReady = false;
	d.ingestConnected = false;
	d.ingestEnded = false;
	d.options = options;
	d.delaySeconds = ClampDelaySeconds(options.delaySeconds);
	++d.delayGen;
	d.keyframeInterval = 0.0;
	d.keyframeCheckNs = 0;
	d.listenUrl = "rtmp://127.0.0.1:" + std::to_string(port) + "/trigglow/live";
	d.sinks.clear();
	for (const Destination &dest : destinations) {
		auto sink = std::make_unique<Impl::Sink>();
		sink->dest = dest;
		sink->status.name = dest.name;
		sink->status.primary = dest.primary;
		d.sinks.push_back(std::move(sink));
	}
	d.running = true;

	d.ingest = std::thread([&d] { d.IngestMain(); });
	if (d.spill)
		d.io = std::thread([&d] { d.IoMain(); });
	for (auto &sink : d.sinks) {
		Impl::Sink *raw = sink.get();
		raw->thread = std::thread([&d, raw] { d.SinkMain(*raw); });
	}
	// OBS connects right after this returns, and fails the stream if nothing
	// listens yet: wait until FFmpeg's listener is up (a few ms), or give up
	// (FFmpeg couldn't listen) so the stream goes out without the relay.
	const auto waitStart = std::chrono::steady_clock::now();
	bool listening = false;
#ifdef _WIN32
	for (;;) {
		if (LoopbackListening(port)) {
			listening = true;
			break;
		}
		bool ended = false;
		{
			std::lock_guard<std::mutex> lock(d.m);
			ended = d.ingestEnded;
		}
		if (ended || std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(3))
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	listening = true;
#endif
	const long long waitedMs = static_cast<long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart)
			.count());
	if (!listening) {
		TRIGGLOW_LOG_ERROR(kComponent, "the relay couldn't listen on 127.0.0.1:%d (%lld ms)", port, waitedMs);
		Stop();
		errorOut = "the relay couldn't listen on this computer";
		return false;
	}

	serverOut = "rtmp://127.0.0.1:" + std::to_string(port) + "/trigglow";
	keyOut = "live";
	TRIGGLOW_LOG_INFO(
		kComponent,
		"relay listening on 127.0.0.1:%d (ready in %lld ms), %zu destination(s), delay %us, keeps %us%s", port,
		waitedMs, destinations.size(), d.delaySeconds, KeepSeconds(d.spill != nullptr, options.keepSeconds),
		d.spill ? " (older seconds on disk)" : " in RAM");
	return true;
}

void StreamRelay::SetDelay(uint32_t seconds)
{
	{
		std::lock_guard<std::mutex> lock(impl_->m);
		// The same delay again would re-splice every destination to the
		// keyframe before it: up to a keyframe interval replayed for nothing.
		if (impl_->delaySeconds == ClampDelaySeconds(seconds))
			return;
		impl_->delaySeconds = ClampDelaySeconds(seconds);
		++impl_->delayGen;
	}
	impl_->cv.notify_all();
}

void StreamRelay::SetKeepSeconds(uint32_t seconds)
{
	std::lock_guard<std::mutex> lock(impl_->m);
	impl_->options.keepSeconds = ClampDelaySeconds(seconds);
}

void StreamRelay::Dump()
{
	{
		// Under the same lock the senders take before Due(): each re-splices
		// (delayGen) before it can send anything that arrived until now.
		std::lock_guard<std::mutex> lock(impl_->m);
		impl_->history.SetSpliceFloor(NowNs() + 1);
		TRIGGLOW_LOG_INFO(kComponent, "dump: the last %us won't be sent", impl_->delaySeconds);
		impl_->delaySeconds = 0;
		++impl_->delayGen;
	}
	impl_->cv.notify_all();
}

uint32_t StreamRelay::Delay() const
{
	std::lock_guard<std::mutex> lock(impl_->m);
	return impl_->delaySeconds;
}

void StreamRelay::Stop()
{
	Impl &d = *impl_;
	if (!d.running)
		return;
	d.stop.store(true);
	{
		std::lock_guard<std::mutex> lock(d.m);
		for (auto &sink : d.sinks)
			sink->abort.store(true);
	}
	d.cv.notify_all();
	d.ioCv.notify_all();
	if (d.ingest.joinable())
		d.ingest.join();
	for (auto &sink : d.sinks) {
		if (sink->thread.joinable())
			sink->thread.join();
	}
	if (d.io.joinable())
		d.io.join();
	std::lock_guard<std::mutex> lock(d.m);
	for (StreamInfo &info : d.streams)
		avcodec_parameters_free(&info.par);
	d.streams.clear();
	d.history.Clear();
	d.history.Store().SetSpill(nullptr, 0);
	// Closing the file deletes it (and with it the only copy of the key).
	d.spill.reset();
	d.sinks.clear();
	d.running = false;
	TRIGGLOW_LOG_INFO(kComponent, "relay stopped");
}

bool StreamRelay::Running() const
{
	std::lock_guard<std::mutex> lock(impl_->m);
	return impl_->running;
}

bool StreamRelay::Finished() const
{
	std::lock_guard<std::mutex> lock(impl_->m);
	if (!impl_->running)
		return true;
	if (!impl_->ingestEnded)
		return false;
	for (const auto &sink : impl_->sinks) {
		if (!sink->finished)
			return false;
	}
	return true;
}

RelayStatus StreamRelay::GetStatus() const
{
	const Impl &d = *impl_;
	std::lock_guard<std::mutex> lock(d.m);
	RelayStatus status;
	status.running = d.running;
	status.ingestConnected = d.ingestConnected;
	status.ingestEnded = d.ingestEnded;
	status.delaySeconds = d.delaySeconds;
	status.historySeconds = d.history.SpanSeconds();
	status.historyBytes = d.history.Bytes();
	status.historyRamBytes = d.history.RamBytes();
	status.historyDiskBytes = d.history.Store().DiskBytes();
	status.spillActive = d.history.Store().Spill() == SpillState::Active;
	status.spillNote = d.spillNote;
	status.instantRaiseSeconds = std::max(d.delaySeconds, d.KeepSecondsLocked());
	status.bitrateMbps = d.history.BytesPerSecond() * 8.0 / 1e6;
	status.keyframeIntervalSeconds = d.keyframeInterval;
	// The first video track (Enhanced Broadcasting sends several: the first
	// is the main quality).
	for (const StreamInfo &info : d.streams) {
		if (info.kind == MediaKind::Video && info.par) {
			status.videoCodec = avcodec_get_name(info.par->codec_id);
			status.width = info.par->width;
			status.height = info.par->height;
			break;
		}
	}
	for (const auto &sink : d.sinks)
		status.sinks.push_back(sink->status);
	return status;
}

std::vector<std::pair<uint32_t, uint32_t>> StreamRelay::VideoSizes(bool &allH264) const
{
	std::lock_guard<std::mutex> lock(impl_->m);
	std::vector<std::pair<uint32_t, uint32_t>> sizes;
	allH264 = impl_->streamsReady;
	for (const StreamInfo &info : impl_->streams) {
		if (info.kind != MediaKind::Video || !info.par)
			continue;
		sizes.emplace_back(static_cast<uint32_t>(info.par->width), static_cast<uint32_t>(info.par->height));
		allH264 = allH264 && info.par->codec_id == AV_CODEC_ID_H264;
	}
	return sizes;
}

bool StreamRelay::SetLoadingClip(std::vector<LoadingVideoTrack> tracks, std::string &why)
{
	Impl &d = *impl_;
	std::vector<StreamInfo> streams;
	{
		std::lock_guard<std::mutex> lock(d.m);
		if (!d.streamsReady) {
			why = "OBS's stream hasn't reached the relay yet";
			return false;
		}
		streams = d.streams; // The parameters live as long as the relay runs.
	}
	auto store = std::make_shared<LoadingStore>();
	size_t videoIndex = 0;
	for (size_t s = 0; s < streams.size(); ++s) {
		const StreamInfo &info = streams[s];
		if (info.kind == MediaKind::Video) {
			if (!info.par || info.par->codec_id != AV_CODEC_ID_H264) {
				why = "the stream isn't H.264";
				return false;
			}
			if (videoIndex >= tracks.size()) {
				why = "no loading clip for every video track";
				return false;
			}
			const LoadingVideoTrack &in = tracks[videoIndex++];
			if (in.width != static_cast<uint32_t>(info.par->width) ||
			    in.height != static_cast<uint32_t>(info.par->height)) {
				why = "the loading clip's size doesn't match the stream's";
				return false;
			}
			FillerTrack track;
			track.stream = static_cast<uint8_t>(s);
			track.kind = MediaKind::Video;
			track.loopMs = in.loopMs;
			for (const LoadingVideoTrack::Frame &frame : in.frames) {
				Packet p;
				p.kind = MediaKind::Video;
				p.stream = track.stream;
				p.dtsMs = frame.dtsMs;
				p.ptsMs = frame.ptsMs;
				p.keyframe = frame.keyframe;
				p.recordOffset = store->payloads.size();
				store->payloads.push_back(AnnexBToAvcc(frame.data));
				p.bytes = static_cast<uint32_t>(store->payloads.back().size());
				track.packets.push_back(p);
			}
			std::vector<uint8_t> header = AvccHeader(in.header);
			if (track.packets.empty() || !track.packets.front().keyframe || track.loopMs <= 0 ||
			    header.empty()) {
				why = "the loading clip is incomplete";
				return false;
			}
			store->filler.tracks.push_back(std::move(track));
			store->headers.push_back(std::move(header));
		} else if (info.kind == MediaKind::Audio) {
			std::vector<uint8_t> frame;
			std::vector<uint8_t> header;
			int samples = 1024;
			if (!SilentAac(info.par, frame, header, samples)) {
				why = "couldn't make silence for the audio";
				return false;
			}
			// A loop of whole milliseconds: n frames of `samples` at the rate.
			const int64_t rate = info.par->sample_rate;
			int64_t n = 1;
			while (n < 512 && (n * samples * 1000) % rate != 0)
				++n;
			n = std::max<int64_t>(n, (2000 * rate) / (samples * 1000LL) / n * n);
			FillerTrack track;
			track.stream = static_cast<uint8_t>(s);
			track.kind = MediaKind::Audio;
			track.loopMs = n * samples * 1000 / rate;
			const uint64_t payload = store->payloads.size();
			store->payloads.push_back(frame);
			for (int64_t i = 0; i < n; ++i) {
				Packet p;
				p.kind = MediaKind::Audio;
				p.stream = track.stream;
				p.dtsMs = p.ptsMs = i * samples * 1000 / rate;
				p.keyframe = true;
				p.recordOffset = payload;
				p.bytes = static_cast<uint32_t>(frame.size());
				track.packets.push_back(p);
			}
			store->filler.tracks.push_back(std::move(track));
			store->headers.push_back(std::move(header));
		}
	}
	if (videoIndex != tracks.size() || store->filler.tracks.empty()) {
		why = "the loading clip's tracks don't match the stream's";
		return false;
	}
	std::shared_ptr<const LoadingStore> shared = store;
	const std::shared_ptr<const Filler> filler(shared, &shared->filler);
	{
		std::lock_guard<std::mutex> lock(d.m);
		d.loading = shared;
		for (auto &sink : d.sinks)
			sink->sched.SetFiller(filler);
	}
	d.cv.notify_all();
	return true;
}

bool StreamRelay::HasLoadingClip() const
{
	std::lock_guard<std::mutex> lock(impl_->m);
	return impl_->loading != nullptr;
}

void StreamRelay::SetEventCallback(EventCallback callback)
{
	std::lock_guard<std::mutex> lock(impl_->m);
	impl_->callback = std::move(callback);
}

#endif

} // namespace trigglow::relay
