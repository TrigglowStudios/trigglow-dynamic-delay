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

#pragma once

// v1.0.0 stream mode: the bytes of an RTMP publisher (rtmp-publisher.hpp),
// with no I/O and no FFmpeg, so they can be unit tested.
//
// Why the relay has its own: FFmpeg's RTMP client keeps 128-byte chunks unless
// the server announces a size, and writes every chunk with two send()s, each
// after a select(). YouTube announces none, so a YouTube destination costs
// ~10 CPU seconds per minute at 6 Mbps instead of ~1 (docs/STREAM_MODE.md,
// "CPU"). The relay announces 4096-byte chunks itself, like OBS, and sends a
// whole batch of packets in a few large writes.
//
//   - AMF0: what the publisher's commands need, and a reader for the
//     server's replies (numbers, strings, objects' string fields).
//   - ChunkWriter: a message as RTMP chunks (a full header, then
//     continuation headers).
//   - ChunkReader: the server's chunks back into messages, fed whatever
//     arrived (it never waits for more).
//   - FlvToRtmp: FFmpeg's FLV muxer output (header, tags) into the messages a
//     publisher sends, the way FFmpeg's rtmp_write does (onMetaData gets
//     "@setDataFrame" in front).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace trigglow::relay::rtmp {

// Message types (RTMP 1.0 spec, 5.4 and 7.1).
enum MessageType : uint8_t {
	kSetChunkSize = 1,
	kAbort = 2,
	kAcknowledgement = 3,
	kUserControl = 4,
	kWindowAckSize = 5,
	kSetPeerBandwidth = 6,
	kAudio = 8,
	kVideo = 9,
	kDataAmf3 = 15,
	kCommandAmf3 = 17,
	kDataAmf0 = 18,
	kCommandAmf0 = 20,
};

// User control events (spec 7.1.7).
constexpr uint16_t kPingRequest = 6;
constexpr uint16_t kPingResponse = 7;

// Chunk streams the publisher uses (FFmpeg's choice).
constexpr uint32_t kCsidControl = 2;
constexpr uint32_t kCsidCommand = 3;
constexpr uint32_t kCsidAudioData = 4;
constexpr uint32_t kCsidVideo = 6;
constexpr uint32_t kCsidStream = 8;

constexpr uint32_t kDefaultChunkSize = 128;
// What the relay announces and sends with, like OBS.
constexpr uint32_t kOutChunkSize = 4096;
constexpr size_t kHandshakeBytes = 1536;
// A message from the server larger than this is a protocol error (they are
// a few hundred bytes; the cap only bounds memory).
constexpr uint32_t kMaxInboundMessage = 4u << 20;

// ---------------------------------------------------------------- AMF0 --

class AmfWriter {
public:
	explicit AmfWriter(std::vector<uint8_t> &out) : out_(out) {}
	void Number(double value);
	void Boolean(bool value);
	void String(const std::string &value);
	void Null();
	void ObjectStart();
	void Field(const std::string &name); // A key inside an object.
	void ObjectEnd();

private:
	void Raw16(const std::string &value);
	std::vector<uint8_t> &out_;
};

struct AmfValue {
	enum class Kind { Number, Boolean, String, Null, Object, Other };
	Kind kind = Kind::Other;
	double number = 0.0;
	bool boolean = false;
	std::string string;
	// An object's (or ECMA array's) top-level string and number fields, the
	// numbers written with %g. Nested values are skipped.
	std::map<std::string, std::string> fields;

	std::string Field(const std::string &name) const;
};

// Reads AMF0 values one after another. False (and stays false) on anything
// malformed or AMF3.
class AmfReader {
public:
	AmfReader(const uint8_t *data, size_t size) : p_(data), end_(data + size) {}
	bool Read(AmfValue &value);
	bool AtEnd() const { return p_ >= end_; }

private:
	bool ReadValue(AmfValue *value, int depth);
	bool ReadProperties(AmfValue *value, int depth);
	bool Take(size_t bytes, const uint8_t *&at);
	const uint8_t *p_;
	const uint8_t *end_;
	bool bad_ = false;
};

// A command message (_result, onStatus...): name, transaction id, the rest.
struct Command {
	std::string name;
	double transaction = 0.0;
	std::vector<AmfValue> args;
};

// Parses a type 20 (or type 17: a 0 byte, then AMF0) message body.
bool ParseCommand(uint8_t type, const uint8_t *data, size_t size, Command &out);

// --------------------------------------------------------------- chunks --

struct Message {
	uint8_t type = 0;
	uint32_t streamId = 0;
	uint32_t timestamp = 0;
	std::vector<uint8_t> payload;
};

// Appends `payload` as one message in `chunkSize` pieces: a full (type 0)
// header, then type 3 headers. Timestamps from 0xFFFFFF on carry the extended
// field on every chunk, as FFmpeg and librtmp write it.
void AppendMessage(std::vector<uint8_t> &out, uint32_t csid, uint8_t type, uint32_t streamId, uint32_t timestamp,
		   const uint8_t *payload, size_t size, uint32_t chunkSize);

// Control and command messages the publisher sends.
void AppendSetChunkSize(std::vector<uint8_t> &out, uint32_t size);
void AppendWindowAckSize(std::vector<uint8_t> &out, uint32_t size);
void AppendAcknowledgement(std::vector<uint8_t> &out, uint32_t bytesReceived);
void AppendPingResponse(std::vector<uint8_t> &out, uint32_t timestamp, const uint8_t *eventData, size_t size,
			uint32_t chunkSize);

class ChunkReader {
public:
	// Bytes as they arrived.
	void Feed(const uint8_t *data, size_t size);
	// The next complete message. False when more bytes are needed, or after
	// an error (Failed()). Set Chunk Size and Abort are applied here and
	// returned too.
	bool Next(Message &out);
	bool Failed() const { return failed_; }
	uint32_t ChunkSize() const { return chunkSize_; }
	size_t Buffered() const { return buffer_.size() - at_; }

private:
	struct Stream {
		uint32_t timestamp = 0;
		uint32_t delta = 0;
		uint32_t length = 0;
		uint8_t type = 0;
		uint32_t streamId = 0;
		bool extended = false;
		bool seen = false;
		std::vector<uint8_t> partial;
	};
	std::vector<uint8_t> buffer_;
	size_t at_ = 0;
	uint32_t chunkSize_ = kDefaultChunkSize;
	std::map<uint32_t, Stream> streams_;
	bool failed_ = false;
};

// ------------------------------------------------------------ FLV tags --

// FFmpeg's FLV muxer output, in any pieces, to RTMP messages: audio on chunk
// stream 4, video on 6, data on 4 (FFmpeg's rtmp_write).
class FlvToRtmp {
public:
	void SetStreamId(uint32_t id) { streamId_ = id; }
	void SetChunkSize(uint32_t size) { chunkSize_ = size; }
	// Appends the chunks of every tag completed by `data` to `out`. False on
	// a malformed FLV stream.
	bool Feed(const uint8_t *data, size_t size, std::vector<uint8_t> &out);
	uint64_t Messages() const { return messages_; }

private:
	std::vector<uint8_t> pending_;
	size_t skip_ = 0; // FLV header bytes still to drop (unknown until read).
	bool headerDone_ = false;
	uint32_t streamId_ = 1;
	uint32_t chunkSize_ = kOutChunkSize;
	uint64_t messages_ = 0;
	std::vector<uint8_t> scratch_;
};

// "rtmp://host/app" or "rtmps://host:443/app/sub/" -> scheme, host, port
// (1935 / 443 when absent), and the app ("app", "app/sub"). False for other
// schemes or a URL without a host. tcUrl is the URL without trailing slashes.
struct ServerUrl {
	bool tls = false;
	std::string host;
	int port = 0;
	std::string app;
	std::string tcUrl;
};
bool ParseServerUrl(const std::string &url, ServerUrl &out);

// Replaces every occurrence of `secret` in `text` (the server's own words in
// an error, which might echo the stream key).
std::string Redact(std::string text, const std::string &secret);

} // namespace trigglow::relay::rtmp
