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

#include "rtmp-wire.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace trigglow::relay::rtmp {

namespace {

void Put8(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v));
}

void PutBe16(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void PutBe24(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void PutBe32(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void PutLe32(std::vector<uint8_t> &out, uint32_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 24));
}

uint32_t Be16(const uint8_t *p)
{
	return (uint32_t(p[0]) << 8) | p[1];
}

uint32_t Be24(const uint8_t *p)
{
	return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}

uint32_t Be32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// The basic header (fmt, chunk stream id), in its 1, 2 or 3 byte form.
void PutBasicHeader(std::vector<uint8_t> &out, uint32_t fmt, uint32_t csid)
{
	if (csid < 64) {
		Put8(out, (fmt << 6) | csid);
	} else if (csid < 64 + 256) {
		Put8(out, fmt << 6);
		Put8(out, csid - 64);
	} else {
		Put8(out, (fmt << 6) | 1);
		Put8(out, (csid - 64) & 0xFF);
		Put8(out, (csid - 64) >> 8);
	}
}

void AppendControl(std::vector<uint8_t> &out, uint8_t type, uint32_t value)
{
	std::vector<uint8_t> body;
	PutBe32(body, value);
	AppendMessage(out, kCsidControl, type, 0, 0, body.data(), body.size(), kDefaultChunkSize);
}

} // namespace

// ---------------------------------------------------------------- AMF0 --

void AmfWriter::Raw16(const std::string &value)
{
	const size_t n = std::min<size_t>(value.size(), 0xFFFF);
	PutBe16(out_, static_cast<uint32_t>(n));
	out_.insert(out_.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(n));
}

void AmfWriter::Number(double value)
{
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	Put8(out_, 0x00);
	for (int shift = 56; shift >= 0; shift -= 8)
		Put8(out_, static_cast<uint32_t>(bits >> shift) & 0xFF);
}

void AmfWriter::Boolean(bool value)
{
	Put8(out_, 0x01);
	Put8(out_, value ? 1 : 0);
}

void AmfWriter::String(const std::string &value)
{
	if (value.size() > 0xFFFF) {
		Put8(out_, 0x0C); // Long string.
		PutBe32(out_, static_cast<uint32_t>(value.size()));
		out_.insert(out_.end(), value.begin(), value.end());
		return;
	}
	Put8(out_, 0x02);
	Raw16(value);
}

void AmfWriter::Null()
{
	Put8(out_, 0x05);
}

void AmfWriter::ObjectStart()
{
	Put8(out_, 0x03);
}

void AmfWriter::Field(const std::string &name)
{
	Raw16(name);
}

void AmfWriter::ObjectEnd()
{
	PutBe16(out_, 0);
	Put8(out_, 0x09);
}

std::string AmfValue::Field(const std::string &name) const
{
	auto it = fields.find(name);
	return it == fields.end() ? std::string() : it->second;
}

bool AmfReader::Take(size_t bytes, const uint8_t *&at)
{
	if (static_cast<size_t>(end_ - p_) < bytes) {
		bad_ = true;
		return false;
	}
	at = p_;
	p_ += bytes;
	return true;
}

bool AmfReader::Read(AmfValue &value)
{
	if (bad_ || AtEnd())
		return false;
	value = AmfValue();
	if (!ReadValue(&value, 0)) {
		bad_ = true;
		return false;
	}
	return true;
}

// Object properties up to the end marker. Into `value->fields` when given.
bool AmfReader::ReadProperties(AmfValue *value, int depth)
{
	for (;;) {
		const uint8_t *at = nullptr;
		if (!Take(2, at))
			return false;
		const uint32_t keyBytes = Be16(at);
		if (keyBytes == 0) {
			if (!Take(1, at))
				return false;
			if (*at == 0x09)
				return true;
			// An empty key with a value: allowed, and skipped.
			--p_;
			if (!ReadValue(nullptr, depth + 1))
				return false;
			continue;
		}
		if (!Take(keyBytes, at))
			return false;
		std::string key(reinterpret_cast<const char *>(at), keyBytes);
		AmfValue field;
		if (!ReadValue(value ? &field : nullptr, depth + 1))
			return false;
		if (value) {
			if (field.kind == AmfValue::Kind::String) {
				value->fields[key] = field.string;
			} else if (field.kind == AmfValue::Kind::Number) {
				char text[32];
				std::snprintf(text, sizeof(text), "%g", field.number);
				value->fields[key] = text;
			}
		}
	}
}

bool AmfReader::ReadValue(AmfValue *value, int depth)
{
	if (depth > 16)
		return false;
	const uint8_t *at = nullptr;
	if (!Take(1, at))
		return false;
	const uint8_t marker = *at;
	AmfValue scratch;
	AmfValue &v = value ? *value : scratch;
	switch (marker) {
	case 0x00: { // Number.
		if (!Take(8, at))
			return false;
		uint64_t bits = 0;
		for (int i = 0; i < 8; ++i)
			bits = (bits << 8) | at[i];
		std::memcpy(&v.number, &bits, sizeof(bits));
		v.kind = AmfValue::Kind::Number;
		return true;
	}
	case 0x01: // Boolean.
		if (!Take(1, at))
			return false;
		v.kind = AmfValue::Kind::Boolean;
		v.boolean = *at != 0;
		return true;
	case 0x02:   // String.
	case 0x0C: { // Long string.
		const size_t lenBytes = marker == 0x02 ? 2 : 4;
		if (!Take(lenBytes, at))
			return false;
		const uint32_t n = lenBytes == 2 ? Be16(at) : Be32(at);
		if (!Take(n, at))
			return false;
		v.kind = AmfValue::Kind::String;
		if (value)
			v.string.assign(reinterpret_cast<const char *>(at), n);
		return true;
	}
	case 0x03: // Object.
		v.kind = AmfValue::Kind::Object;
		return ReadProperties(value, depth);
	case 0x05: // Null.
	case 0x06: // Undefined.
		v.kind = AmfValue::Kind::Null;
		return true;
	case 0x07: // Reference.
		v.kind = AmfValue::Kind::Other;
		return Take(2, at);
	case 0x08: // ECMA array: a count (not to be trusted), then properties.
		if (!Take(4, at))
			return false;
		v.kind = AmfValue::Kind::Object;
		return ReadProperties(value, depth);
	case 0x0A: { // Strict array.
		if (!Take(4, at))
			return false;
		const uint32_t count = Be32(at);
		v.kind = AmfValue::Kind::Other;
		for (uint32_t i = 0; i < count; ++i) {
			if (!ReadValue(nullptr, depth + 1))
				return false;
		}
		return true;
	}
	case 0x0B: // Date: a number and a time zone.
		v.kind = AmfValue::Kind::Other;
		return Take(10, at);
	case 0x0D: // Unsupported.
		v.kind = AmfValue::Kind::Null;
		return true;
	case 0x0F: { // XML document.
		if (!Take(4, at))
			return false;
		v.kind = AmfValue::Kind::Other;
		return Take(Be32(at), at);
	}
	case 0x10: { // Typed object: a class name, then properties.
		if (!Take(2, at))
			return false;
		if (!Take(Be16(at), at))
			return false;
		v.kind = AmfValue::Kind::Object;
		return ReadProperties(value, depth);
	}
	default: // AMF3 (0x11) and anything unknown: can't be skipped.
		return false;
	}
}

bool ParseCommand(uint8_t type, const uint8_t *data, size_t size, Command &out)
{
	if (type == kCommandAmf3) {
		// A format byte (0: AMF0 follows).
		if (size < 1 || data[0] != 0)
			return false;
		++data;
		--size;
	} else if (type != kCommandAmf0) {
		return false;
	}
	AmfReader reader(data, size);
	AmfValue value;
	if (!reader.Read(value) || value.kind != AmfValue::Kind::String)
		return false;
	out = Command();
	out.name = value.string;
	if (reader.Read(value) && value.kind == AmfValue::Kind::Number)
		out.transaction = value.number;
	while (reader.Read(value))
		out.args.push_back(std::move(value));
	return true;
}

// --------------------------------------------------------------- chunks --

void AppendMessage(std::vector<uint8_t> &out, uint32_t csid, uint8_t type, uint32_t streamId, uint32_t timestamp,
		   const uint8_t *payload, size_t size, uint32_t chunkSize)
{
	const bool extended = timestamp >= 0xFFFFFF;
	const size_t step = chunkSize > 0 ? chunkSize : kDefaultChunkSize;
	out.reserve(out.size() + size + 18 + (size / step) * 8);
	PutBasicHeader(out, 0, csid);
	PutBe24(out, extended ? 0xFFFFFF : timestamp);
	PutBe24(out, static_cast<uint32_t>(size));
	Put8(out, type);
	PutLe32(out, streamId);
	if (extended)
		PutBe32(out, timestamp);
	size_t at = 0;
	for (;;) {
		const size_t piece = std::min(step, size - at);
		if (piece > 0)
			out.insert(out.end(), payload + at, payload + at + piece);
		at += piece;
		if (at >= size)
			break;
		PutBasicHeader(out, 3, csid);
		if (extended)
			PutBe32(out, timestamp);
	}
}

void AppendSetChunkSize(std::vector<uint8_t> &out, uint32_t size)
{
	AppendControl(out, kSetChunkSize, size & 0x7FFFFFFF);
}

void AppendWindowAckSize(std::vector<uint8_t> &out, uint32_t size)
{
	AppendControl(out, kWindowAckSize, size);
}

void AppendAcknowledgement(std::vector<uint8_t> &out, uint32_t bytesReceived)
{
	AppendControl(out, kAcknowledgement, bytesReceived);
}

void AppendPingResponse(std::vector<uint8_t> &out, uint32_t timestamp, const uint8_t *eventData, size_t size,
			uint32_t chunkSize)
{
	std::vector<uint8_t> body;
	PutBe16(body, kPingResponse);
	body.insert(body.end(), eventData, eventData + std::min<size_t>(size, 4));
	while (body.size() < 6)
		body.push_back(0);
	AppendMessage(out, kCsidControl, kUserControl, 0, timestamp, body.data(), body.size(), chunkSize);
}

void ChunkReader::Feed(const uint8_t *data, size_t size)
{
	if (failed_ || size == 0)
		return;
	// Drop what's been parsed before growing.
	if (at_ > 0 && at_ >= buffer_.size() / 2) {
		buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at_));
		at_ = 0;
	}
	buffer_.insert(buffer_.end(), data, data + size);
}

bool ChunkReader::Next(Message &out)
{
	while (!failed_) {
		const uint8_t *p = buffer_.data() + at_;
		const size_t avail = buffer_.size() - at_;
		if (avail < 1)
			return false;
		const uint32_t fmt = p[0] >> 6;
		uint32_t csid = p[0] & 0x3F;
		size_t pos = 1;
		if (csid == 0) {
			if (avail < 2)
				return false;
			csid = 64 + p[1];
			pos = 2;
		} else if (csid == 1) {
			if (avail < 3)
				return false;
			csid = 64 + p[1] + (uint32_t(p[2]) << 8);
			pos = 3;
		}
		Stream &s = streams_[csid];
		if (fmt != 0 && !s.seen) {
			failed_ = true; // A compressed header with nothing before it.
			return false;
		}
		static const size_t kHeaderBytes[4] = {11, 7, 3, 0};
		if (avail < pos + kHeaderBytes[fmt])
			return false;
		const uint8_t *h = p + pos;
		uint32_t tsField = 0;
		uint32_t length = s.length;
		uint8_t type = s.type;
		uint32_t streamId = s.streamId;
		if (fmt <= 2)
			tsField = Be24(h);
		if (fmt <= 1) {
			length = Be24(h + 3);
			type = h[6];
		}
		if (fmt == 0)
			streamId = Le32(h + 7);
		pos += kHeaderBytes[fmt];
		const bool extended = fmt <= 2 ? tsField == 0xFFFFFF : s.extended;
		uint32_t extValue = 0;
		if (extended) {
			if (avail < pos + 4)
				return false;
			extValue = Be32(p + pos);
			pos += 4;
		}
		const bool newMessage = s.partial.empty();
		if (fmt <= 1 && !newMessage) {
			failed_ = true; // A new header in the middle of a message.
			return false;
		}
		if (length > kMaxInboundMessage) {
			failed_ = true;
			return false;
		}
		const size_t piece = std::min<size_t>(chunkSize_, length - s.partial.size());
		if (avail < pos + piece)
			return false;

		// The whole chunk is here: commit.
		s.seen = true;
		s.extended = extended;
		s.length = length;
		s.type = type;
		s.streamId = streamId;
		const uint32_t tsValue = extended ? extValue : tsField;
		if (fmt == 0) {
			s.timestamp = tsValue;
			s.delta = 0;
		} else if (fmt <= 2) {
			s.delta = tsValue;
			s.timestamp += tsValue;
		} else if (newMessage) {
			s.timestamp += s.delta;
		}
		s.partial.insert(s.partial.end(), p + pos, p + pos + piece);
		at_ += pos + piece;
		if (s.partial.size() < s.length)
			continue;

		out.type = s.type;
		out.streamId = s.streamId;
		out.timestamp = s.timestamp;
		out.payload.swap(s.partial);
		s.partial.clear();
		if (out.type == kSetChunkSize) {
			if (out.payload.size() < 4) {
				failed_ = true;
				return false;
			}
			const uint32_t size = Be32(out.payload.data()) & 0x7FFFFFFF;
			if (size < 1) {
				failed_ = true;
				return false;
			}
			chunkSize_ = size;
		} else if (out.type == kAbort && out.payload.size() >= 4) {
			auto it = streams_.find(Be32(out.payload.data()));
			if (it != streams_.end())
				it->second.partial.clear();
		}
		return true;
	}
	return false;
}

// ------------------------------------------------------------ FLV tags --

bool FlvToRtmp::Feed(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
	pending_.insert(pending_.end(), data, data + size);
	size_t at = 0;
	if (!headerDone_) {
		// "FLV", version, flags, u32 header size; then u32 previous tag size 0.
		if (pending_.size() < 9)
			return true;
		if (pending_[0] != 'F' || pending_[1] != 'L' || pending_[2] != 'V')
			return false;
		skip_ = static_cast<size_t>(Be32(pending_.data() + 5)) + 4;
		if (skip_ < 13 || skip_ > 1024)
			return false;
		headerDone_ = true;
	}
	if (skip_ > 0) {
		const size_t n = std::min(skip_, pending_.size());
		at += n;
		skip_ -= n;
	}
	while (skip_ == 0 && pending_.size() - at >= 11) {
		const uint8_t *tag = pending_.data() + at;
		const uint8_t type = tag[0] & 0x1F; // Bit 5: the encrypted (filter) flag.
		const uint32_t bytes = Be24(tag + 1);
		const uint32_t timestamp = Be24(tag + 4) | (uint32_t(tag[7]) << 24);
		if (pending_.size() - at < 11 + static_cast<size_t>(bytes) + 4)
			break;
		const uint8_t *body = tag + 11;
		if (type == kAudio || type == kVideo) {
			AppendMessage(out, type == kVideo ? kCsidVideo : kCsidAudioData, type, streamId_, timestamp,
				      body, bytes, chunkSize_);
			++messages_;
		} else if (type == kDataAmf0) {
			// onMetaData (and |RtmpSampleAccess) go out as
			// @setDataFrame(name, value): what servers expect from a
			// publisher, and what FFmpeg's rtmp_write sends.
			AmfReader reader(body, bytes);
			AmfValue first;
			const bool setDataFrame = reader.Read(first) && first.kind == AmfValue::Kind::String &&
						  (first.string == "onMetaData" || first.string == "|RtmpSampleAccess");
			scratch_.clear();
			if (setDataFrame) {
				AmfWriter w(scratch_);
				w.String("@setDataFrame");
			}
			scratch_.insert(scratch_.end(), body, body + bytes);
			AppendMessage(out, kCsidAudioData, kDataAmf0, streamId_, timestamp, scratch_.data(),
				      scratch_.size(), chunkSize_);
			++messages_;
		}
		// Any other tag type is dropped (FFmpeg's client would pass it on);
		// the FLV muxer never writes one.
		at += 11 + static_cast<size_t>(bytes) + 4;
	}
	pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(at));
	return true;
}

// ------------------------------------------------------------------ URL --

bool ParseServerUrl(const std::string &url, ServerUrl &out)
{
	out = ServerUrl();
	const size_t sep = url.find("://");
	if (sep == std::string::npos)
		return false;
	std::string scheme = url.substr(0, sep);
	std::transform(scheme.begin(), scheme.end(), scheme.begin(),
		       [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
	if (scheme == "rtmp") {
		out.tls = false;
		out.port = 1935;
	} else if (scheme == "rtmps") {
		out.tls = true;
		out.port = 443;
	} else {
		return false;
	}
	const size_t start = sep + 3;
	const size_t slash = url.find('/', start);
	std::string authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
	// user:password@ isn't used by streaming services; kept out of the host.
	const size_t atSign = authority.rfind('@');
	if (atSign != std::string::npos)
		authority = authority.substr(atSign + 1);
	std::string host = authority;
	std::string portText;
	if (!authority.empty() && authority[0] == '[') {
		const size_t close = authority.find(']');
		if (close == std::string::npos)
			return false;
		host = authority.substr(1, close - 1);
		if (close + 1 < authority.size()) {
			if (authority[close + 1] != ':')
				return false;
			portText = authority.substr(close + 2);
		}
	} else {
		const size_t colon = authority.find(':');
		if (colon != std::string::npos) {
			host = authority.substr(0, colon);
			portText = authority.substr(colon + 1);
		}
	}
	if (host.empty())
		return false;
	if (!portText.empty()) {
		int port = 0;
		for (char c : portText) {
			if (c < '0' || c > '9')
				return false;
			port = port * 10 + (c - '0');
			if (port > 65535)
				return false;
		}
		if (port <= 0)
			return false;
		out.port = port;
	}
	out.host = host;
	if (slash != std::string::npos) {
		out.app = url.substr(slash + 1);
		while (!out.app.empty() && out.app.back() == '/')
			out.app.pop_back();
	}
	out.tcUrl = url;
	while (out.tcUrl.size() > start && out.tcUrl.back() == '/')
		out.tcUrl.pop_back();
	return true;
}

std::string Redact(std::string text, const std::string &secret)
{
	if (secret.size() < 4)
		return text;
	size_t at = 0;
	while ((at = text.find(secret, at)) != std::string::npos) {
		text.replace(at, secret.size(), "***");
		at += 3;
	}
	return text;
}

} // namespace trigglow::relay::rtmp
