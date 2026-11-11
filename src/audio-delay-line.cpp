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

void AudioDelayLine::Configure(uint32_t channels, uint32_t sampleRate)
{
	if (channels == channels_ && sampleRate == sampleRate_)
		return;
	channels_ = channels;
	sampleRate_ = sampleRate;
	ring_.assign(channels, {});
	capacity_ = 0;
	Reset();
}

void AudioDelayLine::Reset()
{
	written_ = 0;
	valid_ = 0;
	lagFrames_ = targetFrames_;
	fadeIn_ = true;
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

void AudioDelayLine::Reallocate(uint64_t capacity)
{
	const auto newCapacity = static_cast<size_t>(std::max<uint64_t>(1, capacity));
	const uint64_t keep = std::min<uint64_t>(valid_, newCapacity);
	for (auto &channelRing : ring_) {
		std::vector<float> next(newCapacity, 0.0f);
		// Contiguous runs: neither ring wraps inside one.
		for (uint64_t pos = written_ - keep; pos < written_;) {
			const auto from = static_cast<size_t>(pos % capacity_);
			const auto to = static_cast<size_t>(pos % newCapacity);
			const auto run = static_cast<size_t>(
				std::min<uint64_t>({written_ - pos, capacity_ - from, newCapacity - to}));
			std::memcpy(next.data() + to, channelRing.data() + from, run * sizeof(float));
			pos += run;
		}
		channelRing.swap(next);
	}
	capacity_ = newCapacity;
	valid_ = keep;
}

void AudioDelayLine::EnsureCapacity(uint64_t needed)
{
	if (needed <= capacity_)
		return;
	if (capacity_ == 0 || valid_ == 0) {
		capacity_ = static_cast<size_t>(needed + MarginFrames());
		for (auto &channelRing : ring_)
			channelRing.assign(capacity_, 0.0f);
		valid_ = 0;
		return;
	}
	Reallocate(needed + MarginFrames()); // Keeps every frame held.
}

void AudioDelayLine::Write(const float *const *planes, size_t frames)
{
	if (channels_ == 0 || sampleRate_ == 0 || frames == 0)
		return;
	EnsureCapacity(targetFrames_ + frames + MarginFrames());
	const auto start = static_cast<size_t>(written_ % capacity_);
	const size_t firstPart = std::min(frames, capacity_ - start);
	for (uint32_t c = 0; c < channels_; ++c) {
		std::vector<float> &channelRing = ring_[c];
		const float *in = planes ? planes[c] : nullptr;
		if (!in) {
			// Silence rather than samples from a previous lap of the ring.
			std::fill_n(channelRing.begin() + static_cast<std::ptrdiff_t>(start), firstPart, 0.0f);
			if (firstPart < frames)
				std::fill_n(channelRing.begin(), frames - firstPart, 0.0f);
			continue;
		}
		std::memcpy(channelRing.data() + start, in, firstPart * sizeof(float));
		if (firstPart < frames)
			std::memcpy(channelRing.data(), in + firstPart, (frames - firstPart) * sizeof(float));
	}
	written_ += frames;
	valid_ = std::min<uint64_t>(valid_ + frames, capacity_);
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
		float *dst = out[c];
		if (!dst)
			continue;
		std::fill_n(dst, first, 0.0f);
		if (count == 0)
			continue;
		const std::vector<float> &channelRing = ring_[c];
		const auto start =
			static_cast<size_t>(static_cast<uint64_t>(base + static_cast<int64_t>(missing)) % capacity_);
		const size_t firstPart = std::min(count, capacity_ - start);
		std::memcpy(dst + first, channelRing.data() + start, firstPart * sizeof(float));
		if (firstPart < count)
			std::memcpy(dst + first + firstPart, channelRing.data(), (count - firstPart) * sizeof(float));
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
			Reallocate(needed);
	}
	return true;
}

} // namespace trigglow
