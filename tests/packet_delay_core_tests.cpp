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

// Standalone unit tests for trigglow::relay (src/packet-delay-core.hpp), the
// v1.0.0 stream mode core: the packet history and the splicing scheduler.
// Same approach as the other tests: no libobs, no FFmpeg, no framework.

#include "packet-delay-core.hpp"

#include <cstdio>
#include <vector>

using namespace trigglow::relay;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_currentTest = "";

void Check(bool ok, const char *expr, const char *file, int line)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("FAIL [%s] %s:%d: %s\n", g_currentTest, file, line, expr);
	}
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

constexpr int64_t kMs = 1000000; // ns

// A fake 60 fps stream, keyframe every 2 s, AAC audio every ~21 ms, that
// reaches the relay exactly in real time (arrival = dts).
struct FakeStream {
	PacketHistory history;
	int64_t nextVideoMs = 0;
	int64_t nextAudioUs = 0;
	int frame = 0;

	// Pushes everything with dts <= `untilMs`, interleaved by dts.
	void RunUntil(int64_t untilMs)
	{
		for (;;) {
			const int64_t audioMs = nextAudioUs / 1000;
			const bool videoNext = nextVideoMs <= audioMs;
			const int64_t t = videoNext ? nextVideoMs : audioMs;
			if (t > untilMs)
				return;
			Packet p;
			p.dtsMs = t;
			p.ptsMs = t;
			p.arrivalNs = t * kMs;
			p.bytes = videoNext ? 12500 : 400;
			if (videoNext) {
				p.kind = MediaKind::Video;
				p.keyframe = frame % 120 == 0;
				++frame;
				nextVideoMs = frame * 1000 / 60;
			} else {
				p.kind = MediaKind::Audio;
				p.stream = 1;
				nextAudioUs += 21333;
			}
			history.Push(p);
		}
	}
};

struct OutLog {
	std::vector<OutPacket> packets;
	bool monotonic = true;
	int64_t lastVideo = -1000000;
	int64_t lastAudio = -1000000;
	int64_t maxVideoGap = 0;
	int64_t newestArrivalMs = -1;

	void Add(const std::vector<OutPacket> &batch)
	{
		for (const OutPacket &o : batch) {
			if (o.packet->kind == MediaKind::Video) {
				if (o.dtsMs <= lastVideo)
					monotonic = false;
				if (lastVideo > -1000000 && o.dtsMs - lastVideo > maxVideoGap)
					maxVideoGap = o.dtsMs - lastVideo;
				lastVideo = o.dtsMs;
			} else if (o.packet->kind == MediaKind::Audio) {
				if (o.dtsMs <= lastAudio)
					monotonic = false;
				lastAudio = o.dtsMs;
			}
			if (o.packet->arrivalNs / kMs > newestArrivalMs)
				newestArrivalMs = o.packet->arrivalNs / kMs;
			packets.push_back(o);
		}
	}
};

// Runs the stream and the sink together from `fromMs` to `toMs` in 5 ms
// ticks; returns the newest content (arrival) the sink sent at every tick
// minus the tick time, i.e. the smallest delay it ever had.
int64_t Run(FakeStream &s, SinkScheduler &sink, OutLog &log, int64_t fromMs, int64_t toMs)
{
	int64_t minDelayMs = 1 << 30;
	for (int64_t t = fromMs; t <= toMs; t += 5) {
		s.RunUntil(t);
		log.Add(sink.Due(t * kMs, s.history, 100000));
		if (log.newestArrivalMs >= 0 && t - log.newestArrivalMs < minDelayMs)
			minDelayMs = t - log.newestArrivalMs;
	}
	return minDelayMs;
}

void TestLive()
{
	g_currentTest = "live passthrough";
	FakeStream s;
	SinkScheduler sink;
	s.RunUntil(0);
	sink.SetDelay(0, 0, s.history);
	OutLog log;
	Run(s, sink, log, 0, 10000);
	CHECK(log.monotonic);
	CHECK(!log.packets.empty());
	CHECK(log.packets.front().dtsMs == 0);
	CHECK(log.maxVideoGap <= 17);
	CHECK(sink.CaughtUp(s.history));
}

void TestEnableRewindsAndHoldsDelay()
{
	g_currentTest = "enable rewinds";
	FakeStream s;
	SinkScheduler sink;
	s.RunUntil(0);
	sink.SetDelay(0, 0, s.history);
	OutLog log;
	Run(s, sink, log, 0, 30000);
	const int64_t lastBefore = log.lastVideo;

	// Delay on at t=30 s: the sink jumps back to the keyframe at <= 20 s.
	const SpliceInfo info = sink.SetDelay(10000 * kMs, 30000 * kMs, s.history);
	CHECK(info.spliced);
	CHECK(!info.shortHistory);
	CHECK(info.jumpSeconds >= 10.0 && info.jumpSeconds <= 12.1);

	OutLog after;
	after.lastVideo = log.lastVideo;
	after.lastAudio = log.lastAudio;
	const int64_t minDelay = Run(s, sink, after, 30000, 60000);
	CHECK(after.monotonic);
	// The first frame after the splice continues the timeline one frame later.
	CHECK(!after.packets.empty());
	bool foundVideo = false;
	for (const OutPacket &o : after.packets) {
		if (o.packet->kind != MediaKind::Video)
			continue;
		CHECK(o.packet->keyframe);
		CHECK(o.dtsMs > lastBefore && o.dtsMs <= lastBefore + 34);
		foundVideo = true;
		break;
	}
	CHECK(foundVideo);
	// Never anything newer than now - 10 s once the delay is on.
	CHECK(minDelay >= 10000 - 5);
	CHECK(after.maxVideoGap <= 34);
}

void TestDisableSkipsForward()
{
	g_currentTest = "disable skips forward";
	FakeStream s;
	SinkScheduler sink;
	s.RunUntil(0);
	sink.SetDelay(20000 * kMs, 0, s.history);
	OutLog log;
	Run(s, sink, log, 0, 60000);
	CHECK(log.monotonic);
	CHECK(log.newestArrivalMs <= 60000 - 20000);

	const SpliceInfo info = sink.SetDelay(0, 60000 * kMs, s.history);
	CHECK(info.spliced);
	CHECK(info.jumpSeconds < -17.0);
	OutLog after;
	after.lastVideo = log.lastVideo;
	after.lastAudio = log.lastAudio;
	Run(s, sink, after, 60000, 70000);
	CHECK(after.monotonic);
	CHECK(after.newestArrivalMs >= 69900);
}

void TestShortHistoryWaits()
{
	g_currentTest = "short history";
	FakeStream s;
	SinkScheduler sink;
	s.RunUntil(5000);
	const SpliceInfo info = sink.SetDelay(30000 * kMs, 5000 * kMs, s.history);
	CHECK(info.spliced);
	CHECK(info.shortHistory);
	CHECK(sink.Due(5000 * kMs, s.history).empty());
	CHECK(sink.NextDueNs(s.history) == 30000 * kMs);
	OutLog log;
	const int64_t minDelay = Run(s, sink, log, 5005, 40000);
	CHECK(minDelay >= 30000 - 5);
	CHECK(log.monotonic);
}

void TestSpliceBeforeFirstKeyframe()
{
	g_currentTest = "no keyframe yet";
	PacketHistory h;
	Packet a;
	a.kind = MediaKind::Audio;
	h.Push(a);
	Packet v;
	v.kind = MediaKind::Video;
	v.keyframe = false;
	h.Push(v);
	SinkScheduler sink;
	CHECK(!sink.SetDelay(0, 0, h).spliced);
	CHECK(sink.Due(0, h).empty());
	Packet k;
	k.kind = MediaKind::Video;
	k.keyframe = true;
	k.dtsMs = 33;
	k.ptsMs = 33;
	h.Push(k);
	const auto out = sink.Due(0, h);
	CHECK(out.size() == 1);
	CHECK(out.size() == 1 && out[0].packet->keyframe && out[0].dtsMs == 0);
}

void TestTrim()
{
	g_currentTest = "trim";
	FakeStream s;
	s.RunUntil(60000);
	const size_t before = s.history.Bytes();
	s.history.Trim(60000 * kMs, 20000 * kMs, UINT64_MAX);
	CHECK(s.history.Bytes() < before);
	const Packet *front = s.history.At(s.history.FrontSeq());
	CHECK(front && front->kind == MediaKind::Video && front->keyframe);
	CHECK(front && front->arrivalNs <= 40000 * kMs);
	CHECK(front && front->arrivalNs >= 38000 * kMs);
	CHECK(s.history.KeyframeIntervalSeconds() > 1.9 && s.history.KeyframeIntervalSeconds() < 2.1);

	// A sink still at 10 s keeps everything from there.
	FakeStream s2;
	s2.RunUntil(60000);
	uint64_t keep = 0;
	CHECK(s2.history.FindSplicePoint(10000 * kMs, keep));
	s2.history.Trim(60000 * kMs, 20000 * kMs, keep);
	CHECK(s2.history.FrontSeq() <= keep);
}

void TestHelpers()
{
	g_currentTest = "helpers";
	CHECK(ClampDelaySeconds(-3) == 0);
	CHECK(ClampDelaySeconds(30) == 30);
	CHECK(ClampDelaySeconds(9000) == kMaxDelaySeconds);
	CHECK(RetentionNs(30, 120, 2.0) == static_cast<int64_t>((120 + 4 + 1) * 1e9));
	CHECK(RetentionNs(300, 120, 4.0) == static_cast<int64_t>((300 + 8 + 1) * 1e9));
	CHECK(EstimateBytes(6000000.0, 60.0) == 45000000ull);
	CHECK(EstimateBytes(0.0, 60.0) == 0);
}

void TestAudioOnly()
{
	g_currentTest = "audio only";
	PacketHistory h;
	for (int i = 0; i < 100; ++i) {
		Packet a;
		a.kind = MediaKind::Audio;
		a.dtsMs = i * 21;
		a.ptsMs = i * 21;
		a.arrivalNs = i * 21 * kMs;
		h.Push(a);
	}
	SinkScheduler sink;
	CHECK(sink.SetDelay(500 * kMs, 2100 * kMs, h).spliced);
	const auto out = sink.Due(2100 * kMs, h);
	CHECK(!out.empty());
	CHECK(!out.empty() && out.back().packet->arrivalNs <= 1600 * kMs);
}

} // namespace

int main()
{
	TestLive();
	TestEnableRewindsAndHoldsDelay();
	TestDisableSkipsForward();
	TestShortHistoryWaits();
	TestSpliceBeforeFirstKeyframe();
	TestTrim();
	TestHelpers();
	TestAudioOnly();
	std::printf("packet_delay_core_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
