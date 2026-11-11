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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
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
	// x264's default B-frames (as OBS streams them): closed GOPs of 121
	// frames, then mini-GOPs sent P, B-ref, b, b and shown b, B-ref, b, P,
	// with every pts two frames after the decode clock (pts - dts from 0 to
	// 5 frames, as measured on a real OBS stream).
	bool bframes = false;

	int64_t PtsOf(int decodeIndex) const
	{
		if (!bframes)
			return decodeIndex * 1000LL / 60;
		const int g = decodeIndex % 121;
		int display = decodeIndex;
		if (g != 0) {
			static const int kOrder[4] = {3, 1, 0, 2};
			const int base = decodeIndex - g + 4 * ((g - 1) / 4) + 1;
			display = base + kOrder[(g - 1) % 4];
		}
		return (display + 2) * 1000LL / 60;
	}

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
			p.ptsMs = videoNext ? PtsOf(frame) : t;
			p.arrivalNs = t * kMs;
			p.bytes = videoNext ? 12500 : 400;
			if (videoNext) {
				p.kind = MediaKind::Video;
				p.keyframe = frame % (bframes ? 121 : 120) == 0;
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

void TestDumpNeverSendsUnaired()
{
	g_currentTest = "dump";
	FakeStream s;
	SinkScheduler sink;
	s.RunUntil(0);
	sink.SetDelay(10000 * kMs, 0, s.history);
	OutLog log;
	Run(s, sink, log, 0, 40000);
	// At 40 s viewers are at ~30 s; 30-40 s hasn't aired. Dump: floor at
	// now, delay off.
	s.history.SetSpliceFloor(40000 * kMs + 1);
	const SpliceInfo info = sink.SetDelay(0, 40000 * kMs, s.history);
	CHECK(!info.spliced); // No keyframe after the floor yet: wait for it.
	OutLog after;
	after.lastVideo = log.lastVideo;
	after.lastAudio = log.lastAudio;
	Run(s, sink, after, 40000, 50000);
	CHECK(after.monotonic);
	CHECK(!after.packets.empty());
	bool aired = false;
	for (const OutPacket &o : after.packets) {
		if (o.packet->arrivalNs < 40000 * kMs)
			aired = true;
	}
	CHECK(!aired);
	// The first video sent is the first keyframe after the floor (42 s).
	for (const OutPacket &o : after.packets) {
		if (o.packet->kind == MediaKind::Video) {
			CHECK(o.packet->keyframe && o.packet->arrivalNs == 42000 * kMs);
			break;
		}
	}

	// Raising the delay again never rewinds into what was dumped.
	const SpliceInfo again = sink.SetDelay(20000 * kMs, 50000 * kMs, s.history);
	CHECK(again.spliced);
	CHECK(again.shortHistory);
	OutLog later;
	later.lastVideo = after.lastVideo;
	later.lastAudio = after.lastAudio;
	Run(s, sink, later, 50000, 70000);
	for (const OutPacket &o : later.packets)
		CHECK(o.packet->arrivalNs >= 40000 * kMs);
	CHECK(later.monotonic);
}

// B-frames: every splice (delay on, raised, off) starts the new GOP after
// everything already shown, so a player never goes back in display order.
void TestBFramesSpliceKeepsDisplayOrder()
{
	g_currentTest = "B-frames splice";
	FakeStream s;
	s.bframes = true;
	SinkScheduler sink;
	s.RunUntil(0);
	sink.SetDelay(0, 0, s.history);

	struct Segment {
		int64_t until;
		int64_t delayMs;
	};
	// Splices land at odd times, in the middle of mini-GOPs.
	const Segment segments[] = {{30007, 10000}, {50023, 25000}, {70041, 5000}, {80013, 0}, {90000, 0}};
	OutLog log;
	int64_t from = 0;
	int64_t maxPtsBefore = INT64_MIN;
	for (const Segment &seg : segments) {
		OutLog part;
		part.lastVideo = log.lastVideo;
		part.lastAudio = log.lastAudio;
		Run(s, sink, part, from, seg.until);
		CHECK(part.monotonic);
		int64_t minPts = INT64_MAX;
		int64_t maxPts = INT64_MIN;
		for (const OutPacket &o : part.packets) {
			if (o.packet->kind != MediaKind::Video)
				continue;
			minPts = std::min(minPts, o.ptsMs);
			maxPts = std::max(maxPts, o.ptsMs);
			CHECK(o.ptsMs >= o.dtsMs);
		}
		CHECK(minPts != INT64_MAX);
		if (maxPtsBefore != INT64_MIN)
			CHECK(minPts > maxPtsBefore);
		maxPtsBefore = std::max(maxPtsBefore, maxPts);
		log.lastVideo = part.lastVideo;
		log.lastAudio = part.lastAudio;
		// A splice moves the timeline by at most a few frames.
		CHECK(part.maxVideoGap <= 6 * 17);
		sink.SetDelay(seg.delayMs * kMs, seg.until * kMs, s.history);
		from = seg.until;
	}
}

} // namespace

// The history keeps each packet's record in its PayloadStore and frees it
// with the packet.
void TestHistoryRecords()
{
	g_currentTest = "HistoryRecords";
	PacketHistory h;
	std::vector<uint8_t> payload(50000);
	for (size_t i = 0; i < payload.size(); ++i)
		payload[i] = static_cast<uint8_t>(i * 7);
	const uint8_t header[4] = {0xde, 0xad, 0xbe, 0xef};
	for (int i = 0; i < 600; ++i) {
		Packet p;
		p.kind = MediaKind::Video;
		p.keyframe = i % 60 == 0;
		p.dtsMs = p.ptsMs = i * 33;
		p.arrivalNs = i * 33 * kMs;
		p.bytes = 50000;
		payload[0] = static_cast<uint8_t>(i);
		const PayloadStore::Span parts[2] = {{header, 4}, {payload.data(), payload.size()}};
		h.Push(p, parts, 2);
	}
	CHECK(h.Bytes() == 600u * 50000u);
	const Packet *p = h.At(123);
	CHECK(p && p->recordBytes == 50004);
	std::vector<uint8_t> record;
	CHECK(p && h.CopyRecord(*p, record));
	CHECK(record.size() == 50004 && record[0] == 0xde && record[4] == 123 && record[5] == payload[1]);
	const size_t before = h.RamBytes();
	CHECK(before >= 600u * 50004u);
	// Keep about the last 5 s (packets 33 ms apart, keyframes every 60).
	h.Trim(600 * 33 * kMs, 5000 * kMs, UINT64_MAX);
	CHECK(h.FrontSeq() == 420);
	CHECK(h.RamBytes() < before / 2);
	CHECK(h.RamBytes() <= (600u - 420u) * 50004u + 3 * PayloadStore::kDefaultBlockBytes + 256 * 1024);
	p = h.At(500);
	record.clear();
	CHECK(p && h.CopyRecord(*p, record) && record.size() == 50004 && record[4] == static_cast<uint8_t>(500));
	h.Clear();
	CHECK(h.RamBytes() == 0 && h.Bytes() == 0);
}

// The spill's hooks in the history: where a time is in the store, what a
// sender needs next, a sender waiting for bytes that aren't in RAM, and
// packets whose bytes were lost.
class LosingDisk : public SpillBackend {
public:
	std::map<uint64_t, std::vector<uint8_t>> slots;
	bool failReads = false;
	bool WriteSlot(uint64_t slot, uint64_t, const uint8_t *data, size_t bytes) override
	{
		slots[slot].assign(data, data + bytes);
		return true;
	}
	bool ReadSlot(uint64_t slot, uint64_t, uint8_t *data, size_t bytes) override
	{
		if (failReads || slots[slot].size() != bytes)
			return false;
		std::copy(slots[slot].begin(), slots[slot].end(), data);
		return true;
	}
	bool Truncate(uint64_t) override { return true; }
};

void TestSpillHooks()
{
	g_currentTest = "SpillHooks";
	LosingDisk disk;
	PacketHistory h;
	PayloadStore::SpillLimits limits;
	limits.maxBacklogBlocks = 1 << 20;
	h.Store().SetSpill(&disk, 100000, limits);
	std::vector<uint8_t> payload(20000, 0x5a);
	// 30 fps video only, keyframe every 60 frames, 20 kB each: 10 s.
	for (int i = 0; i < 300; ++i) {
		Packet p;
		p.kind = MediaKind::Video;
		p.keyframe = i % 60 == 0;
		p.dtsMs = p.ptsMs = i * 1000 / 30;
		p.arrivalNs = p.dtsMs * kMs;
		p.bytes = 20000;
		const PayloadStore::Span part{payload.data(), payload.size()};
		h.Push(p, &part, 1);
	}
	CHECK(h.OffsetAtArrival(0) == 0);
	CHECK(h.OffsetAtArrival(5000 * kMs) == 150u * 20000u);
	CHECK(h.OffsetAtArrival(5001 * kMs) == 151u * 20000u);
	CHECK(h.OffsetAtArrival(99999 * kMs) == h.Store().EndOffset());
	const PayloadStore::Range ahead = h.AheadRange(30, 1000 * kMs);
	CHECK(ahead.from == 30u * 20000u && ahead.to == 61u * 20000u);

	// Everything older than the last 2 s goes to "disk".
	auto pump = [&](const std::vector<PayloadStore::Range> &needed) {
		for (;;) {
			PayloadStore::IoJob job = h.Store().PlanIo(h.OffsetAtArrival(8000 * kMs), needed);
			if (job.kind == PayloadStore::IoJob::Kind::None)
				return;
			h.Store().RunIo(job);
			h.Store().FinishIo(job);
			h.DropLost();
		}
	};
	pump({});
	CHECK(h.Store().DiskBytes() > 0);
	CHECK(!h.RecordInRam(*h.At(0)));
	CHECK(h.RecordInRam(*h.At(299)));

	// A sink 9.5 s behind: its packets aren't in RAM, so nothing goes and the
	// cursor waits; once read back they go.
	SinkScheduler sink;
	sink.SetDelay(9500 * kMs, 10000 * kMs, h);
	auto ready = [&](const Packet &p) {
		return h.RecordInRam(p);
	};
	CHECK(sink.Due(10000 * kMs, h, 1000, ready).empty());
	CHECK(sink.CursorSeq() == 0);
	pump({h.AheadRange(sink.CursorSeq(), 3000 * kMs)});
	const auto out = sink.Due(10000 * kMs, h, 1000, ready);
	CHECK(out.size() == 16); // Arrived at 0..500 ms.
	CHECK(sink.CursorSeq() == 16);

	// The disk loses what it holds: those packets go, the history starts
	// again at the next keyframe, and the sink re-splices there.
	disk.failReads = true;
	pump({{0, h.Store().EndOffset()}});
	CHECK(h.Store().Spill() == SpillState::Fallback);
	CHECK(h.Store().LostBefore() > 0);
	CHECK(h.FrontSeq() > 0);
	const Packet *front = h.At(h.FrontSeq());
	CHECK(front && front->keyframe && h.RecordInRam(*front));
	CHECK(front && front->recordOffset >= h.Store().LostBefore());
	const auto after = sink.Due(18000 * kMs, h, 1000, ready);
	CHECK(!after.empty() && after.front().packet->keyframe);
}

int main()
{
	TestBFramesSpliceKeepsDisplayOrder();
	TestHistoryRecords();
	TestSpillHooks();
	TestDumpNeverSendsUnaired();
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
