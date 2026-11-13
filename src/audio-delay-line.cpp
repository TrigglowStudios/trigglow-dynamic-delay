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

#include "audio-delay-line.hpp"

#include <algorithm>
#include <cstring>

namespace trigglow {

namespace {
// Released blocks kept for reuse: the steady state releases one and needs one
// every kBlockFrames, so a couple spare means no allocation at all then.
constexpr size_t kMaxSpareBlocks = 4;
// Released blocks freed per Write() at most. Freeing a lowered 600 s delay's
// blocks at once (~1,400 per source) stalled the source's audio thread long
// enough for OBS to add a tick (21 ms) of audio buffering to the whole output
// for the rest of the session (measured in OBS 33); at 4 per chunk they go in
// seconds, never in one call.
constexpr size_t kMaxFreesPerWrite = 4;
} // namespace

void AudioDelayLine::Configure(uint32_t channels, uint32_t sampleRate)
{
	if (channels == channels_ && sampleRate == sampleRate_)
		return;
	channels_ = channels;
	sampleRate_ = sampleRate;
	// Blocks are sized for the channel count: none of them fits any more.
	blocks_.clear();
	spare_.clear();
	retired_.clear();
	firstBlock_ = 0;
	capacity_ = 0;
	Reset();
}

void AudioDelayLine::Reset()
{
	written_ = 0;
	valid_ = 0;
	lagFrames_ = targetFrames_;
	fadeIn_ = true;
	ReleaseOldBlocks(); // Nothing held: every block goes (a few are kept for reuse).
}

void AudioDelayLine::SetDelayFrames(uint64_t frames)
{
	if (frames == targetFrames_)
		return;
	targetFrames_ = frames;
	if (lagFrames_ > frames || valid_ == 0) {
		// Lowered: skip forward at once. Nothing held yet: nothing to keep.
		lagFrames_ = frames;
		fadeIn_ = true;
	}
	// Raised: Read() lets the lag build up, one chunk of silence at a time.
}

void AudioDelayLine::ReleaseBlock(std::unique_ptr<float[]> block)
{
	if (spare_.size() < kMaxSpareBlocks)
		spare_.push_back(std::move(block));
	else
		retired_.push_back(std::move(block)); // Freed a few at a time by Write().
}

void AudioDelayLine::ReleaseOldBlocks()
{
	if (valid_ == 0) {
		while (!blocks_.empty()) {
			ReleaseBlock(std::move(blocks_.front()));
			blocks_.pop_front();
		}
		firstBlock_ = written_ / kBlockFrames;
		return;
	}
	const uint64_t oldestHeld = written_ - valid_;
	while (!blocks_.empty() && (firstBlock_ + 1) * kBlockFrames <= oldestHeld) {
		ReleaseBlock(std::move(blocks_.front()));
		blocks_.pop_front();
		++firstBlock_;
	}
}

float *AudioDelayLine::BlockFor(uint64_t pos)
{
	const uint64_t block = pos / kBlockFrames;
	if (blocks_.empty())
		firstBlock_ = block;
	while (block >= firstBlock_ + blocks_.size()) {
		if (!spare_.empty()) {
			blocks_.push_back(std::move(spare_.back()));
			spare_.pop_back();
		} else {
			// Not zeroed: every frame is written before it can be read.
			blocks_.push_back(
				std::unique_ptr<float[]>(new float[static_cast<size_t>(channels_) * kBlockFrames]));
		}
	}
	return blocks_[static_cast<size_t>(block - firstBlock_)].get();
}

void AudioDelayLine::SetCapacity(uint64_t capacity)
{
	capacity_ = static_cast<size_t>(std::max<uint64_t>(1, capacity));
	valid_ = std::min<uint64_t>(valid_, capacity_);
	ReleaseOldBlocks();
}

void AudioDelayLine::EnsureCapacity(uint64_t needed)
{
	if (needed <= capacity_)
		return;
	if (capacity_ == 0 || valid_ == 0) {
		valid_ = 0;
		SetCapacity(needed + MarginFrames());
		return;
	}
	SetCapacity(needed + MarginFrames()); // Keeps every frame held.
}

void AudioDelayLine::Write(const float *const *planes, size_t frames)
{
	if (channels_ == 0 || sampleRate_ == 0 || frames == 0)
		return;
	EnsureCapacity(targetFrames_ + frames + MarginFrames());
	uint64_t pos = written_;
	size_t done = 0;
	while (done < frames) {
		float *block = BlockFor(pos);
		const auto inBlock = static_cast<size_t>(pos % kBlockFrames);
		const size_t run = std::min(frames - done, kBlockFrames - inBlock);
		for (uint32_t c = 0; c < channels_; ++c) {
			float *dst = block + static_cast<size_t>(c) * kBlockFrames + inBlock;
			const float *in = planes ? planes[c] : nullptr;
			if (in)
				std::memcpy(dst, in + done, run * sizeof(float));
			else
				std::fill_n(dst, run, 0.0f); // Silence rather than an older lap's samples.
		}
		pos += run;
		done += run;
	}
	written_ += frames;
	valid_ = std::min<uint64_t>(valid_ + frames, capacity_);
	ReleaseOldBlocks();
	for (size_t i = 0; i < kMaxFreesPerWrite && !retired_.empty(); ++i)
		retired_.pop_back();
}

void AudioDelayLine::WriteGated(const float *const *planes, size_t frames, bool open)
{
	Write(open ? planes : nullptr, frames);
}

void AudioDelayLine::WriteSilence(size_t frames)
{
	Write(nullptr, frames);
}

bool AudioDelayLine::Read(float *const *out, size_t frames)
{
	if (channels_ == 0 || frames == 0)
		return false;
	// A raise builds up: this chunk's first `hold` frames are silence and the
	// read point doesn't move (the lag grows by exactly what was written).
	size_t hold = 0;
	if (lagFrames_ < targetFrames_) {
		hold = static_cast<size_t>(std::min<uint64_t>(frames, targetFrames_ - lagFrames_));
		lagFrames_ += hold;
	}
	const size_t real = frames - hold;
	// Absolute position of the first real frame; it may lie before the history
	// (still filling) or even before 0.
	const int64_t base =
		static_cast<int64_t>(written_) - static_cast<int64_t>(lagFrames_) - static_cast<int64_t>(real);
	const int64_t oldestHeld = static_cast<int64_t>(written_ - valid_);
	const auto missing = static_cast<size_t>(std::clamp<int64_t>(oldestHeld - base, 0, static_cast<int64_t>(real)));
	const size_t first = hold + missing; // First output frame with a real sample.
	const size_t count = frames - first;

	for (uint32_t c = 0; c < channels_; ++c) {
		if (out[c])
			std::fill_n(out[c], first, 0.0f);
	}
	// Held frames only: every block from oldestHeld on is there.
	uint64_t pos = static_cast<uint64_t>(base + static_cast<int64_t>(missing));
	size_t done = 0;
	while (done < count) {
		const float *block = blocks_[static_cast<size_t>(pos / kBlockFrames - firstBlock_)].get();
		const auto inBlock = static_cast<size_t>(pos % kBlockFrames);
		const size_t run = std::min(count - done, kBlockFrames - inBlock);
		for (uint32_t c = 0; c < channels_; ++c) {
			if (out[c])
				std::memcpy(out[c] + first + done,
					    block + static_cast<size_t>(c) * kBlockFrames + inBlock,
					    run * sizeof(float));
		}
		pos += run;
		done += run;
	}

	if (count == 0) {
		fadeIn_ = true;
		return false;
	}
	if (fadeIn_) {
		// Out of silence, or a jump, straight into mid-waveform audio is a
		// hard step (a click): 10 ms fade-in.
		const size_t fade = std::min<size_t>(count, std::max<uint32_t>(1, sampleRate_ / 100));
		for (uint32_t c = 0; c < channels_; ++c) {
			if (!out[c])
				continue;
			for (size_t i = 0; i < fade; ++i)
				out[c][first + i] *= static_cast<float>(i) / static_cast<float>(fade);
		}
		fadeIn_ = false;
	}

	// A lowered delay that has settled no longer needs the room of the old
	// one: give back what's well past it (at least 10 s of samples).
	if (lagFrames_ == targetFrames_) {
		const uint64_t needed = targetFrames_ + frames + 2 * MarginFrames();
		if (capacity_ > 2 * needed && capacity_ - needed > 10ULL * sampleRate_)
			SetCapacity(needed);
	}
	return true;
}

} // namespace trigglow
