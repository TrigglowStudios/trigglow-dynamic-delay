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

// Standalone unit tests for src/rtmp-wire.hpp: the stream mode relay's own
// RTMP bytes. No sockets, no FFmpeg, no framework (check.hpp).

#include "rtmp-wire.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace trigglow::relay::rtmp;

namespace {

std::vector<uint8_t> Bytes(std::initializer_list<int> list)
{
	std::vector<uint8_t> out;
	for (int b : list)
		out.push_back(static_cast<uint8_t>(b));
	return out;
}

std::vector<uint8_t> Pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> out(n);
	std::mt19937 rng(seed);
	for (auto &b : out)
		b = static_cast<uint8_t>(rng());
	return out;
}

// Every message the reader gets out of `bytes`, fed in pieces of `step`.
std::vector<Message> ReadAll(ChunkReader &reader, const std::vector<uint8_t> &bytes, size_t step)
{
	std::vector<Message> out;
	for (size_t at = 0; at < bytes.size(); at += step) {
		reader.Feed(bytes.data() + at, std::min(step, bytes.size() - at));
		Message msg;
		while (reader.Next(msg))
			out.push_back(msg);
	}
	return out;
}

void TestAmf()
{
	testcheck::g_currentTest = "amf";
	std::vector<uint8_t> body;
	AmfWriter w(body);
	w.String("_result");
	w.Number(4);
	w.Null();
	w.ObjectStart();
	w.Field("level");
	w.String("status");
	w.Field("code");
	w.String("NetStream.Publish.Start");
	w.Field("clientid");
	w.Number(1234.5);
	w.Field("ok");
	w.Boolean(true);
	w.ObjectEnd();
	w.Number(1);

	// The exact bytes of a string and a number.
	CHECK(std::vector<uint8_t>(body.begin(), body.begin() + 10) ==
	      Bytes({0x02, 0x00, 0x07, '_', 'r', 'e', 's', 'u', 'l', 't'}));
	CHECK(std::vector<uint8_t>(body.begin() + 10, body.begin() + 19) ==
	      Bytes({0x00, 0x40, 0x10, 0, 0, 0, 0, 0, 0}));

	Command cmd;
	CHECK(ParseCommand(kCommandAmf0, body.data(), body.size(), cmd));
	CHECK(cmd.name == "_result");
	CHECK(cmd.transaction == 4.0);
	CHECK_EQ(cmd.args.size(), 3u);
	if (cmd.args.size() == 3) {
		CHECK(cmd.args[0].kind == AmfValue::Kind::Null);
		CHECK(cmd.args[1].kind == AmfValue::Kind::Object);
		CHECK(cmd.args[1].Field("code") == "NetStream.Publish.Start");
		CHECK(cmd.args[1].Field("level") == "status");
		CHECK(cmd.args[1].Field("clientid") == "1234.5");
		CHECK(cmd.args[1].Field("missing").empty());
		CHECK(cmd.args[2].kind == AmfValue::Kind::Number && cmd.args[2].number == 1.0);
	}

	// Type 17: a format byte, then AMF0.
	std::vector<uint8_t> amf3 = {0};
	amf3.insert(amf3.end(), body.begin(), body.end());
	CHECK(ParseCommand(kCommandAmf3, amf3.data(), amf3.size(), cmd) && cmd.name == "_result");
	CHECK(!ParseCommand(kAudio, body.data(), body.size(), cmd));

	// ECMA arrays, nested objects, strict arrays, dates, long strings,
	// typed objects: skipped or read, the values after them still read.
	std::vector<uint8_t> mixed;
	AmfWriter m(mixed);
	m.String("onStatus");
	m.Number(0);
	mixed.insert(mixed.end(), {0x08, 0, 0, 0, 2});         // ECMA array, count 2.
	mixed.insert(mixed.end(), {0, 4, 'c', 'o', 'd', 'e'}); // "code":
	m.String("X.Y");
	mixed.insert(mixed.end(), {0, 3, 'o', 'b', 'j', 0x03, 0, 1, 'a', 0x05, 0, 0, 0x09}); // "obj": {a: null}
	mixed.insert(mixed.end(), {0, 0, 0x09});
	mixed.insert(mixed.end(), {0x0A, 0, 0, 0, 2, 0x05, 0x01, 0x01});          // [null, true]
	mixed.insert(mixed.end(), {0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});          // Date.
	mixed.insert(mixed.end(), {0x0C, 0, 0, 0, 3, 'a', 'b', 'c'});             // Long string.
	mixed.insert(mixed.end(), {0x10, 0, 1, 'T', 0, 1, 'k', 0x02, 0, 1, 'v'}); // Typed object.
	mixed.insert(mixed.end(), {0, 0, 0x09});
	m.Number(7);
	CHECK(ParseCommand(kCommandAmf0, mixed.data(), mixed.size(), cmd));
	CHECK_EQ(cmd.args.size(), 6u);
	if (cmd.args.size() == 6) {
		CHECK(cmd.args[0].kind == AmfValue::Kind::Object && cmd.args[0].Field("code") == "X.Y");
		CHECK(cmd.args[3].kind == AmfValue::Kind::String && cmd.args[3].string == "abc");
		CHECK(cmd.args[4].Field("k") == "v");
		CHECK(cmd.args[5].number == 7.0);
	}

	// Malformed: every truncation stops cleanly; AMF3 values stop reading.
	for (size_t cut = 0; cut < body.size(); ++cut) {
		AmfReader reader(body.data(), cut);
		AmfValue v;
		int values = 0;
		while (reader.Read(v))
			++values;
		CHECK(values <= 4);
	}
	const std::vector<uint8_t> amf3Value = Bytes({0x11, 0x06, 0x03, 'a'});
	AmfReader reader(amf3Value.data(), amf3Value.size());
	AmfValue v;
	CHECK(!reader.Read(v));
	// A runaway nesting depth.
	std::vector<uint8_t> deep;
	for (int i = 0; i < 40; ++i)
		deep.insert(deep.end(), {0x03, 0, 1, 'x'});
	AmfReader deepReader(deep.data(), deep.size());
	CHECK(!deepReader.Read(v));
}

void TestChunkWriter()
{
	testcheck::g_currentTest = "chunk writer";
	const std::vector<uint8_t> small = Pattern(10, 1);
	std::vector<uint8_t> out;
	AppendMessage(out, 4, kAudio, 1, 0x123456, small.data(), small.size(), 4096);
	std::vector<uint8_t> want = Bytes({0x04, 0x12, 0x34, 0x56, 0, 0, 10, 8, 1, 0, 0, 0});
	want.insert(want.end(), small.begin(), small.end());
	CHECK(out == want);

	// 10000 bytes in 4096-byte chunks: 12 + 4096, 1 + 4096, 1 + 1808.
	const std::vector<uint8_t> big = Pattern(10000, 2);
	out.clear();
	AppendMessage(out, 6, kVideo, 1, 40, big.data(), big.size(), 4096);
	CHECK_EQ(out.size(), 12u + 10000u + 2u);
	CHECK_EQ(out[12 + 4096], 0xC6u);
	CHECK_EQ(out[12 + 4096 + 1 + 4096], 0xC6u);
	CHECK(std::memcmp(out.data() + 12, big.data(), 4096) == 0);
	CHECK(std::memcmp(out.data() + 12 + 4097, big.data() + 4096, 4096) == 0);

	// Exactly one chunk's worth: no empty continuation.
	out.clear();
	AppendMessage(out, 6, kVideo, 1, 40, big.data(), 4096, 4096);
	CHECK_EQ(out.size(), 12u + 4096u);
	// An empty message: just the header.
	out.clear();
	AppendMessage(out, 3, kCommandAmf0, 0, 0, nullptr, 0, 4096);
	CHECK_EQ(out.size(), 12u);

	// Extended timestamp: 0xFFFFFF in the header, the value after it and
	// after every continuation header.
	out.clear();
	AppendMessage(out, 6, kVideo, 1, 0x01000000, big.data(), 5000, 4096);
	CHECK(out[1] == 0xFF && out[2] == 0xFF && out[3] == 0xFF);
	CHECK(std::vector<uint8_t>(out.begin() + 12, out.begin() + 16) == Bytes({1, 0, 0, 0}));
	CHECK_EQ(out.size(), 16u + 5000u + 5u);
	CHECK_EQ(out[16 + 4096], 0xC6u);
	CHECK(std::vector<uint8_t>(out.begin() + 16 + 4097, out.begin() + 16 + 4101) == Bytes({1, 0, 0, 0}));

	// Chunk stream ids past 63: two and three byte basic headers.
	out.clear();
	AppendMessage(out, 64 + 10, kAudio, 1, 0, small.data(), small.size(), 4096);
	CHECK(out[0] == 0x00 && out[1] == 10);
	out.clear();
	AppendMessage(out, 64 + 300, kAudio, 1, 0, small.data(), small.size(), 4096);
	CHECK(out[0] == 0x01 && out[1] == (300 & 0xFF) && out[2] == (300 >> 8));

	out.clear();
	AppendSetChunkSize(out, 4096);
	CHECK(out == Bytes({0x02, 0, 0, 0, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0, 0x10, 0}));
	out.clear();
	const std::vector<uint8_t> pingData = Bytes({0, 0, 0x30, 0x39});
	AppendPingResponse(out, 0, pingData.data(), pingData.size(), 4096);
	CHECK(out == Bytes({0x02, 0, 0, 0, 0, 0, 6, 4, 0, 0, 0, 0, 0, 7, 0, 0, 0x30, 0x39}));
}

void TestChunkReaderRoundTrip()
{
	testcheck::g_currentTest = "chunk reader round trip";
	// Messages as the relay writes them, read back whole with every feed size.
	struct Spec {
		uint32_t csid;
		uint8_t type;
		uint32_t ts;
		size_t bytes;
	};
	const Spec specs[] = {{4, kAudio, 0, 300},         {6, kVideo, 33, 9000},          {4, kAudio, 21, 4096},
			      {6, kVideo, 0xFFFFFF, 5000}, {6, kVideo, 0x7FFFFFFF, 12000}, {3, kCommandAmf0, 0, 0},
			      {64 + 200, kAudio, 5, 77}};
	for (uint32_t chunk : {128u, 4096u, 60000u}) {
		std::vector<uint8_t> bytes;
		AppendSetChunkSize(bytes, chunk);
		std::vector<std::vector<uint8_t>> payloads;
		uint32_t seed = 10;
		for (const Spec &s : specs) {
			payloads.push_back(Pattern(s.bytes, seed++));
			AppendMessage(bytes, s.csid, s.type, 1, s.ts, payloads.back().data(), s.bytes, chunk);
		}
		for (size_t step : {size_t(1), size_t(7), size_t(1000), bytes.size()}) {
			ChunkReader reader;
			const std::vector<Message> got = ReadAll(reader, bytes, step);
			CHECK(!reader.Failed());
			CHECK_EQ(reader.ChunkSize(), chunk);
			CHECK_EQ(got.size(), 1u + std::size(specs));
			for (size_t i = 0; i + 1 < got.size() && i < std::size(specs); ++i) {
				CHECK_EQ(got[i + 1].type, specs[i].type);
				CHECK_EQ(got[i + 1].timestamp, specs[i].ts);
				CHECK_EQ(got[i + 1].streamId, 1u);
				CHECK(got[i + 1].payload == payloads[i]);
			}
			CHECK_EQ(reader.Buffered(), 0u);
		}
	}
}

void TestChunkReaderServerStyle()
{
	testcheck::g_currentTest = "chunk reader compressed headers";
	// What a server may send: compressed headers (fmt 1, 2, 3), two chunk
	// streams interleaved, 128-byte chunks.
	std::vector<uint8_t> bytes;
	// csid 3, fmt 0: ts 1000, 200 bytes, type 20, stream 0 -- first chunk.
	const std::vector<uint8_t> a = Pattern(200, 5);
	bytes.insert(bytes.end(), {0x03, 0, 0x03, 0xE8, 0, 0, 200, 20, 0, 0, 0, 0});
	bytes.insert(bytes.end(), a.begin(), a.begin() + 128);
	// csid 2, fmt 0, interleaved: window ack size 2500000.
	bytes.insert(bytes.end(), {0x02, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0, 0x00, 0x26, 0x25, 0xA0});
	// csid 3, fmt 3: the rest of the first message.
	bytes.push_back(0xC3);
	bytes.insert(bytes.end(), a.begin() + 128, a.end());
	// csid 3, fmt 2: delta 10, same length and type.
	const std::vector<uint8_t> b = Pattern(200, 6);
	bytes.insert(bytes.end(), {0x83, 0, 0, 10});
	bytes.insert(bytes.end(), b.begin(), b.begin() + 128);
	bytes.push_back(0xC3);
	bytes.insert(bytes.end(), b.begin() + 128, b.end());
	// csid 3, fmt 3 starting a new message: delta 10 again.
	bytes.push_back(0xC3);
	bytes.insert(bytes.end(), b.begin(), b.begin() + 128);
	bytes.push_back(0xC3);
	bytes.insert(bytes.end(), b.begin() + 128, b.end());
	// csid 3, fmt 1: delta 5, 4 bytes, type 4 (a ping request).
	bytes.insert(bytes.end(), {0x43, 0, 0, 5, 0, 0, 6, 4, 0, 6, 0, 0, 0x12, 0x34});

	for (size_t step : {size_t(1), size_t(3), bytes.size()}) {
		ChunkReader reader;
		const std::vector<Message> got = ReadAll(reader, bytes, step);
		CHECK(!reader.Failed());
		CHECK_EQ(got.size(), 5u);
		if (got.size() != 5)
			continue;
		CHECK_EQ(got[0].type, uint8_t(kWindowAckSize));
		CHECK(got[1].payload == a && got[1].timestamp == 1000 && got[1].type == kCommandAmf0);
		CHECK(got[2].payload == b && got[2].timestamp == 1010);
		CHECK(got[3].payload == b && got[3].timestamp == 1020);
		CHECK(got[4].type == kUserControl && got[4].timestamp == 1025);
		CHECK(got[4].payload == Bytes({0, 6, 0, 0, 0x12, 0x34}));
	}

	// Set Chunk Size takes effect for the chunks right after it.
	{
		std::vector<uint8_t> s;
		AppendSetChunkSize(s, 300);
		const std::vector<uint8_t> c = Pattern(250, 7);
		AppendMessage(s, 3, kCommandAmf0, 0, 0, c.data(), c.size(), 300);
		ChunkReader reader;
		const std::vector<Message> got = ReadAll(reader, s, s.size());
		CHECK(!reader.Failed());
		CHECK(got.size() == 2 && got[1].payload == c);
	}

	// Abort drops the half-read message on that chunk stream.
	{
		std::vector<uint8_t> s;
		const std::vector<uint8_t> c = Pattern(200, 8);
		s.insert(s.end(), {0x03, 0, 0, 0, 0, 0, 200, 20, 0, 0, 0, 0});
		s.insert(s.end(), c.begin(), c.begin() + 128);
		s.insert(s.end(), {0x02, 0, 0, 0, 0, 0, 4, 2, 0, 0, 0, 0, 0, 0, 0, 3}); // Abort csid 3.
		const std::vector<uint8_t> d = Pattern(20, 9);
		s.insert(s.end(), {0x03, 0, 0, 0, 0, 0, 20, 20, 0, 0, 0, 0});
		s.insert(s.end(), d.begin(), d.end());
		ChunkReader reader;
		const std::vector<Message> got = ReadAll(reader, s, 5);
		CHECK(!reader.Failed());
		CHECK(got.size() == 2 && got[0].type == kAbort && got[1].payload == d);
	}

	// Extended timestamps on fmt 3 chunks after an extended header.
	{
		std::vector<uint8_t> s;
		const std::vector<uint8_t> c = Pattern(200, 10);
		AppendMessage(s, 6, kVideo, 1, 0x02000000, c.data(), c.size(), 128);
		ChunkReader reader;
		const std::vector<Message> got = ReadAll(reader, s, 1);
		CHECK(!reader.Failed());
		CHECK(got.size() == 1 && got[0].timestamp == 0x02000000 && got[0].payload == c);
	}
}

void TestChunkReaderMalformed()
{
	testcheck::g_currentTest = "chunk reader malformed";
	{
		// A compressed header on a chunk stream never seen.
		ChunkReader reader;
		const std::vector<uint8_t> s = Bytes({0x45, 0, 0, 0, 0, 0, 4, 20});
		Message msg;
		reader.Feed(s.data(), s.size());
		CHECK(!reader.Next(msg));
		CHECK(reader.Failed());
	}
	{
		// A message larger than the cap.
		ChunkReader reader;
		const std::vector<uint8_t> s = Bytes({0x03, 0, 0, 0, 0xFF, 0xFF, 0xFF, 20, 0, 0, 0, 0});
		Message msg;
		reader.Feed(s.data(), s.size());
		CHECK(!reader.Next(msg));
		CHECK(reader.Failed());
	}
	{
		// Set Chunk Size 0.
		ChunkReader reader;
		const std::vector<uint8_t> s = Bytes({0x02, 0, 0, 0, 0, 0, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0});
		Message msg;
		reader.Feed(s.data(), s.size());
		CHECK(!reader.Next(msg));
		CHECK(reader.Failed());
	}
	{
		// Random bytes never crash it; it fails or waits.
		for (uint32_t seed = 0; seed < 200; ++seed) {
			ChunkReader reader;
			const std::vector<uint8_t> junk = Pattern(3000, 100 + seed);
			Message msg;
			reader.Feed(junk.data(), junk.size());
			int n = 0;
			while (reader.Next(msg) && n < 10000)
				++n;
			CHECK(n < 10000);
		}
	}
}

// An FLV file's bytes: header, then tags.
void FlvHeader(std::vector<uint8_t> &out)
{
	out.insert(out.end(), {'F', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0});
}

void FlvTag(std::vector<uint8_t> &out, uint8_t type, uint32_t ts, const std::vector<uint8_t> &body)
{
	const uint32_t n = static_cast<uint32_t>(body.size());
	out.insert(out.end(), {type, uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n), uint8_t(ts >> 16), uint8_t(ts >> 8),
			       uint8_t(ts), uint8_t(ts >> 24), 0, 0, 0});
	out.insert(out.end(), body.begin(), body.end());
	const uint32_t prev = n + 11;
	out.insert(out.end(), {uint8_t(prev >> 24), uint8_t(prev >> 16), uint8_t(prev >> 8), uint8_t(prev)});
}

void TestFlvToRtmp()
{
	testcheck::g_currentTest = "flv to rtmp";
	std::vector<uint8_t> meta;
	AmfWriter w(meta);
	w.String("onMetaData");
	w.ObjectStart();
	w.Field("width");
	w.Number(1920);
	w.ObjectEnd();
	std::vector<uint8_t> cue;
	AmfWriter cw(cue);
	cw.String("onCuePoint");
	cw.Null();

	const std::vector<uint8_t> audio = Pattern(400, 20);
	const std::vector<uint8_t> video = Pattern(30000, 21);
	const std::vector<uint8_t> late = Pattern(5000, 22);
	std::vector<uint8_t> flv;
	FlvHeader(flv);
	FlvTag(flv, kDataAmf0, 0, meta);
	FlvTag(flv, kAudio, 0, audio);
	FlvTag(flv, kVideo, 33, video);
	FlvTag(flv, kDataAmf0, 40, cue);
	FlvTag(flv, kVideo, 0x01000010, late); // Past 4.66 hours: the extended byte.

	std::vector<uint8_t> metaWant;
	AmfWriter mw(metaWant);
	mw.String("@setDataFrame");
	metaWant.insert(metaWant.end(), meta.begin(), meta.end());

	for (size_t step : {size_t(1), size_t(5), size_t(4096), flv.size()}) {
		FlvToRtmp conv;
		conv.SetStreamId(7);
		std::vector<uint8_t> rtmpBytes;
		AppendSetChunkSize(rtmpBytes, 4096);
		bool ok = true;
		for (size_t at = 0; at < flv.size(); at += step)
			ok = ok && conv.Feed(flv.data() + at, std::min(step, flv.size() - at), rtmpBytes);
		CHECK(ok);
		CHECK_EQ(conv.Messages(), 5u);
		ChunkReader reader;
		const std::vector<Message> got = ReadAll(reader, rtmpBytes, rtmpBytes.size());
		CHECK(!reader.Failed());
		CHECK_EQ(got.size(), 6u);
		if (got.size() != 6)
			continue;
		CHECK(got[1].type == kDataAmf0 && got[1].payload == metaWant && got[1].streamId == 7);
		CHECK(got[2].type == kAudio && got[2].payload == audio && got[2].timestamp == 0);
		CHECK(got[3].type == kVideo && got[3].payload == video && got[3].timestamp == 33);
		CHECK(got[4].type == kDataAmf0 && got[4].payload == cue && got[4].timestamp == 40);
		CHECK(got[5].type == kVideo && got[5].payload == late && got[5].timestamp == 0x01000010);
	}
	// The chunk streams FFmpeg uses: audio and data on 4, video on 6.
	{
		FlvToRtmp conv;
		std::vector<uint8_t> out;
		CHECK(conv.Feed(flv.data(), flv.size(), out));
		CHECK_EQ(out[0], 0x04u);
	}
	// Not FLV.
	{
		FlvToRtmp conv;
		std::vector<uint8_t> out;
		const std::vector<uint8_t> junk = Bytes({'M', 'P', '4', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0});
		CHECK(!conv.Feed(junk.data(), junk.size(), out));
	}
}

void TestServerUrl()
{
	testcheck::g_currentTest = "server url";
	ServerUrl u;
	CHECK(ParseServerUrl("rtmp://live.twitch.tv/app", u));
	CHECK(!u.tls && u.host == "live.twitch.tv" && u.port == 1935 && u.app == "app");
	CHECK(u.tcUrl == "rtmp://live.twitch.tv/app");
	CHECK(ParseServerUrl("rtmps://a.rtmps.youtube.com:443/live2/", u));
	CHECK(u.tls && u.host == "a.rtmps.youtube.com" && u.port == 443 && u.app == "live2");
	CHECK(u.tcUrl == "rtmps://a.rtmps.youtube.com:443/live2");
	CHECK(ParseServerUrl("RTMPS://fa723fc1b171.global-contribute.live-video.net/app/", u));
	CHECK(u.tls && u.port == 443 && u.app == "app");
	CHECK(ParseServerUrl("rtmp://example.com:1936/live/sub?token=1", u));
	CHECK(u.port == 1936 && u.app == "live/sub?token=1");
	CHECK(ParseServerUrl("rtmp://[::1]:1940/live", u));
	CHECK(u.host == "::1" && u.port == 1940 && u.app == "live");
	CHECK(ParseServerUrl("rtmp://example.com", u));
	CHECK(u.app.empty() && u.tcUrl == "rtmp://example.com");
	CHECK(!ParseServerUrl("srt://example.com:9000", u));
	CHECK(!ParseServerUrl("rtmpt://example.com/live", u));
	CHECK(!ParseServerUrl("rtmp:///live", u));
	CHECK(!ParseServerUrl("rtmp://example.com:99999/live", u));
	CHECK(!ParseServerUrl("rtmp://example.com:12a/live", u));
	CHECK(!ParseServerUrl("example.com/live", u));

	CHECK(Redact("bad name live_123456_abcdef", "live_123456_abcdef") == "bad name ***");
	CHECK(Redact("no key here", "live_123456_abcdef") == "no key here");
	CHECK(Redact("x abc x", "abc") == "x abc x"); // Too short to look for.
}

} // namespace

int main()
{
	TestAmf();
	TestChunkWriter();
	TestChunkReaderRoundTrip();
	TestChunkReaderServerStyle();
	TestChunkReaderMalformed();
	TestFlvToRtmp();
	TestServerUrl();
	return testcheck::Finish("rtmp_wire_tests");
}
