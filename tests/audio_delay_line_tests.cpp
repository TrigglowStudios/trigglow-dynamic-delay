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

// Unit tests for trigglow::AudioDelayLine (src/audio-delay-line.hpp, v1.0.0):
// the delay itself, filling, a lowered delay skipping forward at once, a
// raised one holding (silence) and then carrying on from exactly where it
// stopped, growing and shrinking without losing history, resets, gaps.
//
// Each input sample is its own absolute index (+1, so 0 means silence), which
// makes "which sample came out" a simple comparison. The fade-in after
// silence or a jump scales the first 10 ms; checks skip those frames.

#include "audio-delay-line.hpp"
#include "check.hpp"

#include <vector>

using trigglow::AudioDelayLine;

namespace {

constexpr uint32_t kRate = 1000; // 1 kHz: a "second" is 1000 frames, the fade 10.
constexpr size_t kChunk = 100;
constexpr size_t kFade = kRate / 100;

struct Driver {
	AudioDelayLine line;
	uint64_t next = 0; // Absolute index of the next input frame.
	std::vector<float> outL, outR;
	bool open = true; // The gate (WriteGated): closed, the chunk goes in as silence.

	explicit Driver(uint32_t channels = 2) { line.Configure(channels, kRate); }

	// One chunk in, one chunk out; returns the left channel.
	const std::vector<float> &Step(size_t frames = kChunk)
	{
		std::vector<float> inL(frames), inR(frames);
		for (size_t i = 0; i < frames; ++i) {
			inL[i] = static_cast<float>(next + i + 1);
			inR[i] = -static_cast<float>(next + i + 1);
		}
		const float *planes[2] = {inL.data(), inR.data()};
		line.WriteGated(planes, frames, open);
		next += frames;
		outL.assign(frames, -1.0f);
		outR.assign(frames, -1.0f);
		float *out[2] = {outL.data(), outR.data()};
		line.Read(out, frames);
		return outL;
	}
};

// The input frame that should come out at output position i of the last
// chunk for a delay of `lag` frames (0 = silence expected).
float Expected(const Driver &d, size_t i, uint64_t lag, size_t frames = kChunk)
{
	const int64_t pos = static_cast<int64_t>(d.next) - static_cast<int64_t>(frames) + static_cast<int64_t>(i) -
			    static_cast<int64_t>(lag);
	return pos < 0 ? 0.0f : static_cast<float>(pos + 1);
}

bool ChunkIs(const Driver &d, uint64_t lag, size_t skipFirst = 0)
{
	for (size_t i = skipFirst; i < d.outL.size(); ++i) {
		if (d.outL[i] != Expected(d, i, lag) || d.outR[i] != -Expected(d, i, lag))
			return false;
	}
	return true;
}

bool ChunkSilent(const Driver &d, size_t from = 0, size_t to = SIZE_MAX)
{
	for (size_t i = from; i < d.outL.size() && i < to; ++i) {
		if (d.outL[i] != 0.0f || d.outR[i] != 0.0f)
			return false;
	}
	return true;
}

void TestDelayAndFill()
{
	testcheck::g_currentTest = "delay and fill";
	Driver d;
	d.line.SetDelayFrames(1000);
	// The first second is silence (nothing old enough yet).
	for (int i = 0; i < 10; ++i) {
		d.Step();
		CHECK(ChunkSilent(d));
	}
	d.Step(); // First real chunk (faded in).
	CHECK(ChunkIs(d, 1000, kFade));
	for (int i = 0; i < 20; ++i) {
		d.Step();
		CHECK(ChunkIs(d, 1000));
	}
	CHECK(d.line.CapacityFrames() >= 1000 + kChunk);
	CHECK(d.line.HistoryFrames() >= 1000 + kChunk);
}

void TestPassthrough()
{
	testcheck::g_currentTest = "zero delay";
	Driver d;
	d.line.SetDelayFrames(0);
	d.Step();
	CHECK(ChunkIs(d, 0, kFade));
	d.Step();
	CHECK(ChunkIs(d, 0));
}

void TestLowerSkipsForward()
{
	testcheck::g_currentTest = "lowered: skips forward";
	Driver d;
	d.line.SetDelayFrames(3000);
	for (int i = 0; i < 50; ++i)
		d.Step();
	CHECK(ChunkIs(d, 3000));
	d.line.SetDelayFrames(1000);
	CHECK_EQ(d.line.LagFrames(), 1000u);
	d.Step();
	CHECK(ChunkIs(d, 1000, kFade)); // At once, faded in.
	for (int i = 0; i < 5; ++i) {
		d.Step();
		CHECK(ChunkIs(d, 1000));
	}
}

void TestRaiseHolds()
{
	testcheck::g_currentTest = "raised: holds, then carries on";
	Driver d;
	d.line.SetDelayFrames(1000);
	for (int i = 0; i < 30; ++i)
		d.Step();
	CHECK(ChunkIs(d, 1000));
	// The last sample that came out before the raise.
	const float lastOut = d.outL.back();
	d.line.SetDelayFrames(1550); // 550 frames more: five and a half chunks of hold.
	CHECK_EQ(d.line.LagFrames(), 1000u);
	for (int i = 0; i < 5; ++i) {
		d.Step();
		CHECK(ChunkSilent(d));
	}
	CHECK_EQ(d.line.LagFrames(), 1500u);
	d.Step(); // 50 frames of silence, then the audio exactly where it stopped.
	CHECK(ChunkSilent(d, 0, 50));
	CHECK_EQ(d.line.LagFrames(), 1550u);
	CHECK(d.outL[50 + kFade] == lastOut + 1.0f + static_cast<float>(kFade));
	CHECK(ChunkIs(d, 1550, 50 + kFade));
	d.Step();
	CHECK(ChunkIs(d, 1550));
	// Nothing was lost: the first sample after the hold follows the last one.
	CHECK(Expected(d, 0, 1550) == lastOut + 1.0f + 50.0f);
}

void TestRaiseThenLowerWhileHolding()
{
	testcheck::g_currentTest = "raised then lowered while holding";
	Driver d;
	d.line.SetDelayFrames(1000);
	for (int i = 0; i < 30; ++i)
		d.Step();
	d.line.SetDelayFrames(5000);
	for (int i = 0; i < 10; ++i)
		d.Step();
	CHECK_EQ(d.line.LagFrames(), 2000u);
	// Lowered below where the hold reached: plays at the new delay at once.
	d.line.SetDelayFrames(1500);
	d.Step();
	CHECK(ChunkIs(d, 1500, kFade));
	// Lowered to between where it is and the target: the hold just stops early.
	d.line.SetDelayFrames(4000);
	for (int i = 0; i < 3; ++i)
		d.Step();
	CHECK_EQ(d.line.LagFrames(), 1800u);
	d.line.SetDelayFrames(1900);
	d.Step();
	CHECK(ChunkSilent(d, 0, 100));
	CHECK_EQ(d.line.LagFrames(), 1900u);
	d.Step();
	CHECK(ChunkIs(d, 1900, kFade));
}

void TestGrowKeepsHistoryAndShrinks()
{
	testcheck::g_currentTest = "grow keeps history, shrink after a drop";
	Driver d;
	d.line.SetDelayFrames(500);
	for (int i = 0; i < 20; ++i)
		d.Step();
	const size_t small = d.line.CapacityFrames();
	d.line.SetDelayFrames(60000); // A minute at 1 kHz: the ring has to grow.
	d.Step();
	CHECK(d.line.CapacityFrames() >= 60000u + kChunk);
	CHECK(d.line.HistoryFrames() >= 2000u); // Kept across the growth.
	for (int i = 0; i < 600; ++i)
		d.Step();
	CHECK(ChunkIs(d, 60000));
	// Back down: the ring gives the room back once it has settled.
	d.line.SetDelayFrames(500);
	d.Step();
	CHECK(ChunkIs(d, 500, kFade));
	d.Step();
	CHECK(ChunkIs(d, 500));
	CHECK(d.line.CapacityFrames() < 10000u);
	CHECK(d.line.CapacityFrames() >= small / 2);
	for (int i = 0; i < 5; ++i) {
		d.Step();
		CHECK(ChunkIs(d, 500));
	}
}

void TestReset()
{
	testcheck::g_currentTest = "reset";
	Driver d;
	d.line.SetDelayFrames(300);
	for (int i = 0; i < 10; ++i)
		d.Step();
	d.line.Reset();
	CHECK_EQ(d.line.HistoryFrames(), 0u);
	d.Step();
	CHECK(ChunkSilent(d)); // Nothing old enough after a reset.
	d.Step();
	d.Step();
	d.Step(); // 400 frames written since the reset: the last 100 are 300 old.
	CHECK(!ChunkSilent(d));
	// A raise with no history jumps straight to the new delay.
	d.line.Reset();
	d.line.SetDelayFrames(700);
	CHECK_EQ(d.line.LagFrames(), 700u);
}

void TestSilenceAndMissingPlanes()
{
	testcheck::g_currentTest = "gaps and missing planes";
	AudioDelayLine line;
	line.Configure(2, kRate);
	line.SetDelayFrames(200);
	std::vector<float> ones(kChunk, 1.0f);
	const float *planes[2] = {ones.data(), nullptr}; // Right plane missing: silence.
	std::vector<float> l(kChunk), r(kChunk);
	float *out[2] = {l.data(), r.data()};
	for (int i = 0; i < 3; ++i) {
		line.Write(planes, kChunk);
		line.Read(out, kChunk);
	}
	line.Write(planes, kChunk);
	CHECK(line.Read(out, kChunk));
	CHECK(l[kChunk - 1] == 1.0f && r[kChunk - 1] == 0.0f);
	line.WriteSilence(kChunk); // A gap in the source.
	line.Write(planes, kChunk);
	line.Read(out, kChunk);
	line.Write(planes, kChunk);
	line.Read(out, kChunk);
	CHECK(l[kChunk - 1] == 0.0f); // The gap comes out 200 frames later.
	// Channel changes empty it.
	line.Configure(1, kRate);
	CHECK_EQ(line.HistoryFrames(), 0u);
	CHECK_EQ(line.Channels(), 1u);
}

// v1.0.0: the history's memory comes block by block as it's written, never
// the whole delay at once on the audio thread (a 600 s delay used to zero-fill
// 115 MB per channel at its first chunk), follows a lowered delay down, and a
// Reset gives it back but for a few spare blocks.
void TestMemoryComesInBlocks()
{
	testcheck::g_currentTest = "memory in blocks";
	constexpr uint32_t kObsRate = 48000;
	constexpr size_t kObsChunk = 1024;
	constexpr size_t kBlock = AudioDelayLine::kBlockFrames;
	AudioDelayLine line;
	line.Configure(2, kObsRate);
	line.SetDelayFrames(600ULL * kObsRate);
	std::vector<float> l(kObsChunk), r(kObsChunk), outL(kObsChunk), outR(kObsChunk);
	const float *in[2] = {l.data(), r.data()};
	float *out[2] = {outL.data(), outR.data()};
	uint64_t next = 0;
	auto step = [&] {
		for (size_t i = 0; i < kObsChunk; ++i) {
			l[i] = static_cast<float>((next + i) % 100000);
			r[i] = -l[i];
		}
		line.Write(in, kObsChunk);
		next += kObsChunk;
		line.Read(out, kObsChunk);
	};
	step();
	CHECK(line.CapacityFrames() >= 600u * kObsRate);
	CHECK(line.AllocatedFrames() <= kBlock); // One block, not 600 s of samples.
	bool bounded = true;
	for (int i = 0; i < 3000; ++i) { // ~64 s
		step();
		bounded = bounded && line.AllocatedFrames() <= line.HistoryFrames() + 6 * kBlock;
	}
	CHECK(bounded);
	// A raise moves a number: no new memory beyond the next block.
	const size_t before = line.AllocatedFrames();
	line.SetDelayFrames(900ULL * kObsRate);
	step();
	CHECK(line.AllocatedFrames() <= before + kBlock);
	// Lowered to 5 s while 600 s of room were kept: once it settles, the room
	// and the blocks follow it down.
	line.SetDelayFrames(5ULL * kObsRate);
	for (int i = 0; i < 300; ++i) // ~6 s
		step();
	CHECK(line.CapacityFrames() < 20u * kObsRate);
	CHECK(line.AllocatedFrames() <= line.CapacityFrames() + 6 * kBlock);
	// What comes out is still what went in exactly 5 s before.
	CHECK(outL[kObsChunk - 1] == static_cast<float>((next - 1 - 5ULL * kObsRate) % 100000));
	CHECK(outR[kObsChunk - 1] == -outL[kObsChunk - 1]);
	// The blocks a lowering or a Reset releases are freed a few per chunk,
	// never all in one call (a stall on the source's audio thread).
	const size_t held = line.AllocatedFrames();
	line.Reset();
	CHECK(line.AllocatedFrames() == held);
	step();
	CHECK(line.AllocatedFrames() + 4 * kBlock >= held);
	for (int i = 0; i < 200; ++i)
		step();
	CHECK(line.AllocatedFrames() <= line.HistoryFrames() + 6 * kBlock);
}

} // namespace

// v1.0.0, several delayed scenes: the gate closes and opens where the samples
// go in, so the source's sound stops and starts exactly one delay later.
void TestGateSwitchesOneDelayLater()
{
	testcheck::g_currentTest = "gate switches one delay later";
	Driver d;
	d.line.SetDelayFrames(1000);
	for (int i = 0; i < 20; ++i)
		d.Step(); // Filled and playing.
	CHECK(ChunkIs(d, 1000));

	// Closed at 2000: what went in before still plays until 2999...
	d.open = false;
	for (int i = 0; i < 10; ++i) {
		d.Step();
		CHECK(ChunkIs(d, 1000));
	}
	// ...then silence, for as long as it stays closed and one delay after.
	for (int i = 0; i < 10; ++i) {
		d.Step();
		CHECK(ChunkSilent(d));
	}

	// Opened at 4000: still silent until 4999 (it was closed from 3000)...
	d.open = true;
	for (int i = 0; i < 10; ++i) {
		d.Step();
		CHECK(ChunkSilent(d));
	}
	// ...then the source again from 4000, one delay late, faded in.
	d.Step();
	CHECK(ChunkIs(d, 1000, kFade));
	for (int i = 0; i < 10; ++i) {
		d.Step();
		CHECK(ChunkIs(d, 1000));
	}
}

int main()
{
	TestDelayAndFill();
	TestPassthrough();
	TestLowerSkipsForward();
	TestRaiseHolds();
	TestRaiseThenLowerWhileHolding();
	TestGrowKeepsHistoryAndShrinks();
	TestReset();
	TestSilenceAndMissingPlanes();
	TestMemoryComesInBlocks();
	TestGateSwitchesOneDelayLater();
	return testcheck::Finish("audio_delay_line_tests");
}
