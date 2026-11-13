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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

// v1.0.0: the delay line behind AudioDelayFilter, pure C++ (no OBS) so
// tests/audio_delay_line_tests.cpp can drive it. Planar float samples, one
// ring per channel, positions counted in absolute frames since the last
// Reset().
//
// The delay can change while it runs, the same way the video ring's does
// (bufferfit::ComputeLiveChange()), so audio stays in step with the picture:
//   - Lowered: the read point jumps forward at once (the samples in between
//     are skipped), with a short fade-in.
//   - Raised: the read point stays where it was and silence comes out until
//     the extra delay has built up (the video holds its last frame for the
//     same time), then the audio carries on from exactly where it stopped.
// History is never thrown away by a change: the ring grows (keeping it) when
// the delay needs more room and shrinks once a lowered delay has settled.
//
// v1.0.0: the history lives in fixed blocks (kBlockFrames per channel), added
// as samples are written and released as they leave it. Before, the ring was
// one array per channel sized for the whole delay: turning the delay on at
// 600 s zero-filled 115 MB per channel, and a raise copied the history into a
// bigger one, on the source's own audio thread (a microphone's or desktop
// capture's) -- a stall of tens of ms per source. Now a raise only moves a
// number; memory comes one block at a time.
//
// Single-threaded: AudioDelayFilter calls everything from OBS's audio thread
// (the delay comes in through an atomic it reads at each chunk).
namespace trigglow {

class AudioDelayLine {
public:
	// Extra room on top of the delay and the chunk, so a write can never
	// reach a sample still to be read.
	static constexpr uint32_t kMarginSeconds = 1;
	// Frames per channel in a storage block (~0.34 s at 48 kHz, 64 KB a channel).
	static constexpr size_t kBlockFrames = 16384;

	// Channels and sample rate; a change empties the history.
	void Configure(uint32_t channels, uint32_t sampleRate);
	uint32_t Channels() const { return channels_; }
	uint32_t SampleRate() const { return sampleRate_; }

	// The delay to reach, in frames (see the class comment).
	void SetDelayFrames(uint64_t frames);
	uint64_t TargetFrames() const { return targetFrames_; }
	// The delay the next Read() starts at: lags TargetFrames() while a raise
	// builds up.
	uint64_t LagFrames() const { return lagFrames_; }

	// Appends `frames` samples per channel (a null plane, or null planes,
	// write silence), growing the ring first if it can't hold the delay.
	void Write(const float *const *planes, size_t frames);
	// A gap in the source's timeline, filled with silence.
	void WriteSilence(size_t frames);
	// v1.0.0, several delayed scenes: Write() while `open`, silence while
	// not. The gate acts where the samples go IN, so what the line already
	// holds still plays out: closed at T, the source is heard until T + delay
	// and silent after; opened at T, silent until T + delay, then heard from T
	// on -- in step with the delayed video's switch between scenes.
	void WriteGated(const float *const *planes, size_t frames, bool open);
	// The `frames` samples due now (call after the Write() of the same chunk)
	// into out[0..Channels()-1]: silence where there's no history (filling,
	// or a raise building up). True if any real sample came out.
	bool Read(float *const *out, size_t frames);
	// Forgets the history (a source restarted, a timestamp went back); the
	// delay reached becomes the target's at once.
	void Reset();

	// The history the line keeps room for (frames per channel), and what it
	// holds of it.
	size_t CapacityFrames() const { return capacity_; }
	uint64_t HistoryFrames() const { return valid_; }
	// Frames per channel in the blocks allocated right now (spares and blocks
	// waiting to be freed included).
	size_t AllocatedFrames() const { return (blocks_.size() + spare_.size() + retired_.size()) * kBlockFrames; }

private:
	void EnsureCapacity(uint64_t needed);
	// The history kept becomes at most `capacity` frames (the newest).
	void SetCapacity(uint64_t capacity);
	uint64_t MarginFrames() const { return static_cast<uint64_t>(kMarginSeconds) * sampleRate_; }
	// The block holding absolute frame `pos`, allocated (or reused) if new.
	float *BlockFor(uint64_t pos);
	// Releases the blocks wholly older than the history, and every block
	// when there's none.
	void ReleaseOldBlocks();
	void ReleaseBlock(std::unique_ptr<float[]> block);

	uint32_t channels_ = 0;
	uint32_t sampleRate_ = 0;
	// blocks_[i] holds absolute frames [(firstBlock_ + i) * kBlockFrames, +kBlockFrames),
	// planar: channel c at c * kBlockFrames.
	std::deque<std::unique_ptr<float[]>> blocks_;
	uint64_t firstBlock_ = 0;
	std::vector<std::unique_ptr<float[]>> spare_;   // A few released blocks, reused before allocating.
	std::vector<std::unique_ptr<float[]>> retired_; // Released beyond the spares, freed a few per Write().
	size_t capacity_ = 0;
	uint64_t written_ = 0; // Absolute frames written since Reset().
	uint64_t valid_ = 0;   // The newest frames of history held, <= capacity_.
	uint64_t targetFrames_ = 0;
	uint64_t lagFrames_ = 0;
	bool fadeIn_ = true; // The next real sample after silence or a jump fades in.
};

} // namespace trigglow
