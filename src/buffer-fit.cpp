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

#include "buffer-fit.hpp"

#include <algorithm>
#include <cmath>

namespace trigglow::bufferfit {

uint64_t Nv12FrameBytes(uint32_t width, uint32_t height)
{
	// NV12: Y plane (w*h) + interleaved UV at half resolution (w*h/2).
	return (static_cast<uint64_t>(width) * height * 3) / 2;
}

uint64_t SlackFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames)
{
	if (fpsNum == 0 || fpsDen == 0 || delaySeconds == 0)
		return 0;
	// A quarter second of slack (ceil(fps / 4)): playback is by capture time,
	// and capture jitter must never leave the oldest slot younger than the
	// requested delay. With an inter-frame codec, at least a GOP: the key a
	// restarted decoder needs for the frame due is then never overwritten yet.
	const uint64_t quarter = (static_cast<uint64_t>(fpsNum) + 4ULL * fpsDen - 1) / (4ULL * fpsDen);
	return std::max<uint64_t>(quarter, keyframeIntervalFrames);
}

uint64_t DesiredFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames)
{
	if (fpsNum == 0 || fpsDen == 0)
		return 0;
	// ceil(delay * fps) in integer math, so 60/1 stays exactly 1800 for 30s
	// and 60000/1001 gives 1799 (not a float-rounded 1798 or 1800).
	const uint64_t delayFrames = (static_cast<uint64_t>(delaySeconds) * fpsNum + fpsDen - 1) / fpsDen;
	// +1 so a full N-second delay has a slot old enough to read from.
	return delayFrames + SlackFrames(delaySeconds, fpsNum, fpsDen, keyframeIntervalFrames) + 1;
}

uint32_t KeyframeIntervalFrames(uint32_t fpsNum, uint32_t fpsDen)
{
	if (fpsNum == 0 || fpsDen == 0)
		return 0;
	return static_cast<uint32_t>((static_cast<uint64_t>(fpsNum) + 2ULL * fpsDen - 1) / (2ULL * fpsDen));
}

const char *RingClassName(RingClass ringClass)
{
	switch (ringClass) {
	case RingClass::Intra:
		return "intra";
	case RingClass::IntraWithGop:
		return "intra-gop";
	case RingClass::Inter:
		return "inter";
	}
	return "intra";
}

RingClass ParseRingClass(std::string_view name)
{
	if (name == "inter")
		return RingClass::Inter;
	if (name == "intra-gop")
		return RingClass::IntraWithGop;
	return RingClass::Intra;
}

void ApplyStorageClass(Input &in, RingClass ringClass, double intraRatio)
{
	in.compressionRatio = ringClass == RingClass::Inter ? kInterCompressionRatio : intraRatio;
	in.keyframeIntervalFrames = ringClass == RingClass::Intra ? 0 : KeyframeIntervalFrames(in.fpsNum, in.fpsDen);
}

uint64_t FrameIntervalNs(uint32_t fpsNum, uint32_t fpsDen)
{
	if (fpsNum == 0)
		return 0;
	return 1000000000ULL * fpsDen / fpsNum;
}

uint64_t FrameClockNs(uint64_t videoFrameNs, uint64_t wallNs)
{
	// A frame time is never far from the wall clock in OBS: rendering runs at
	// most a few frames late. Anything else isn't the same clock.
	constexpr uint64_t kMaxSkewNs = 2000000000ULL;
	if (videoFrameNs == 0)
		return wallNs;
	const uint64_t skew = videoFrameNs > wallNs ? videoFrameNs - wallNs : wallNs - videoFrameNs;
	return skew <= kMaxSkewNs ? videoFrameNs : wallNs;
}

bool CaptureDue(uint64_t frameNs, uint64_t lastCaptureNs, uint64_t frameIntervalNs, uint64_t captureIntervalNs)
{
	if (lastCaptureNs == 0 || frameNs < lastCaptureNs)
		return true;
	return frameNs - lastCaptureNs + frameIntervalNs / 2 >= captureIntervalNs;
}

double OutputCapScale(const Input &in)
{
	// Escape hatch (buffer_output_cap = false): v0.5.0 sizing, no cap.
	if (!in.capToOutput)
		return 1.0;
	// Unknown geometry: no cap (same as pre-0.5.1).
	if (in.baseWidth == 0 || in.baseHeight == 0 || in.outputWidth == 0 || in.outputHeight == 0)
		return 1.0;
	// Only when the target is canvas-sized (the live scene). A scene from
	// another canvas, or any non-canvas-sized target, is left alone.
	if (in.sourceWidth != in.baseWidth || in.sourceHeight != in.baseHeight)
		return 1.0;
	// Uniform scale, max() of the two axes: keeps the canvas aspect ratio and
	// never goes below the output on either axis (errs towards quality when
	// the canvas and output aspect ratios differ).
	const double sx = static_cast<double>(in.outputWidth) / in.baseWidth;
	const double sy = static_cast<double>(in.outputHeight) / in.baseHeight;
	return std::min(1.0, std::max(sx, sy));
}

uint32_t EvenDim(uint32_t orig, double scale)
{
	// llround, not truncation: 3840 * (720 / 2160) can land just below 1280
	// in floating point, and truncating would give 1278 instead of 1280.
	long long v = std::llround(static_cast<double>(orig) * scale);
	if (v > static_cast<long long>(orig))
		v = static_cast<long long>(orig);
	// Deliberately NOT clamped back to orig: NV12 needs even dimensions of at
	// least 2 (one chroma sample), so a 1-pixel source still gets a 2-pixel
	// buffer (the capture just stretches it). Returning 0 instead would make
	// Compute() report invalid input, and the filter would then pass that
	// source through undelayed while its audio stays delayed.
	if (v < 2)
		v = 2;
	// NV12 chroma subsampling needs even dimensions.
	return static_cast<uint32_t>(v) & ~1u;
}

uint32_t AlignedWidth(uint32_t orig, double scale)
{
	constexpr long long kAlign = kWidthAlignment;
	long long v = std::llround(static_cast<double>(orig) * scale);
	if (v > static_cast<long long>(orig))
		v = static_cast<long long>(orig);
	// Nearest multiple (keeps the aspect ratio closest), but never above the
	// source: OpenGL needs whole 4-byte rows in every plane (header comment).
	long long aligned = (v + kAlign / 2) / kAlign * kAlign;
	if (aligned > static_cast<long long>(orig))
		aligned = static_cast<long long>(orig) / kAlign * kAlign;
	if (aligned < kAlign)
		aligned = kAlign;
	return static_cast<uint32_t>(aligned);
}

Result Compute(const Input &in)
{
	if (in.sourceWidth == 0 || in.sourceHeight == 0 || in.fpsNum == 0 || in.fpsDen == 0)
		return {};

	const double ratio = in.compressionRatio > 0.0 ? in.compressionRatio : 1.0;
	const double budget = static_cast<double>(VideoBudgetBytes(in));
	const uint64_t desired = DesiredFrames(in.delaySeconds, in.fpsNum, in.fpsDen, in.keyframeIntervalFrames);
	const double srcH = static_cast<double>(in.sourceHeight);

	const double capScale = OutputCapScale(in);
	// The quality floor never exceeds the output ceiling.
	const double floorScale = std::min(capScale, std::min(1.0, static_cast<double>(in.minResolutionHeight) / srcH));

	auto bytesAt = [&](double s) {
		return static_cast<double>(
			       Nv12FrameBytes(AlignedWidth(in.sourceWidth, s), EvenDim(in.sourceHeight, s))) /
		       ratio;
	};
	auto fits = [&](double s) {
		return static_cast<double>(desired) * bytesAt(s) <= budget;
	};

	double scale = capScale;
	if (!fits(capScale)) {
		// RAM reduction, computed against the continuous (unrounded) cost so
		// it doesn't depend on the ceiling's rounding, then clamped to
		// [floor, ceiling].
		const double fullResBytes =
			static_cast<double>(Nv12FrameBytes(in.sourceWidth, in.sourceHeight)) / ratio;
		const double ideal = std::sqrt(budget / (static_cast<double>(desired) * fullResBytes));
		const double target = std::clamp(ideal, floorScale, capScale);

		// Walk whole source rows downwards from the target until the rounded
		// frame really fits. llround can round a dimension up past the ideal
		// scale, and without this a RAM-reduced ring could come out a frame or
		// two short of the full delay. Stepping on a fixed row grid keeps the
		// result monotonic in the budget and in the ceiling. Usually 0-2 steps.
		const auto floorRows = static_cast<uint32_t>(std::llround(floorScale * srcH));
		auto rows = static_cast<uint32_t>(std::floor(target * srcH + 1e-6));
		rows = std::max(rows, floorRows);
		while (rows > floorRows && !fits(static_cast<double>(rows) / srcH))
			--rows;
		scale = rows > floorRows ? static_cast<double>(rows) / srcH : floorScale;
	}

	Result r;
	r.bufferWidth = AlignedWidth(in.sourceWidth, scale);
	r.bufferHeight = EvenDim(in.sourceHeight, scale);
	r.desiredFrames = desired;
	const double perFrame = static_cast<double>(Nv12FrameBytes(r.bufferWidth, r.bufferHeight)) / ratio;
	const double framesThatFit = budget / perFrame; // perFrame > 0: width >= 8, height >= 2.
	r.actualFrames = framesThatFit >= static_cast<double>(desired)
				 ? desired
				 : std::max<uint64_t>(1, static_cast<uint64_t>(framesThatFit));
	r.scale = scale;
	r.capScale = capScale;
	r.cappedByOutput = capScale < 1.0;
	r.reducedByRam = scale < capScale;
	r.fitsFullDuration = r.actualFrames >= desired;
	r.effectiveDelaySeconds =
		EffectiveSecondsFor(r.actualFrames, in.delaySeconds, in.fpsNum, in.fpsDen, in.keyframeIntervalFrames);
	return r;
}

uint32_t EffectiveSecondsFor(uint64_t frames, uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen,
			     uint32_t keyframeIntervalFrames)
{
	if (fpsNum == 0 || fpsDen == 0 || frames == 0)
		return 0;
	if (frames >= DesiredFrames(delaySeconds, fpsNum, fpsDen, keyframeIntervalFrames))
		return delaySeconds;
	// Whole seconds, in integer math (frames * den / num), keeping the slack in
	// front of the delay when there's room for it.
	const uint64_t span = frames - 1;
	const uint64_t slack = SlackFrames(delaySeconds, fpsNum, fpsDen, keyframeIntervalFrames);
	const uint64_t withMargin = span > slack ? span - slack : 0;
	uint64_t seconds = withMargin * fpsDen / fpsNum;
	if (seconds == 0)
		seconds = span * fpsDen / fpsNum;
	return static_cast<uint32_t>(std::min<uint64_t>(seconds, delaySeconds));
}

uint64_t VideoBudgetBytes(const Input &in)
{
	const uint64_t audio = in.reservedBytesPerSecond * in.delaySeconds;
	const uint64_t quarter = in.budgetBytes / 4;
	return in.budgetBytes > audio && in.budgetBytes - audio > quarter ? in.budgetBytes - audio : quarter;
}

uint64_t CapacityFrames(uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames)
{
	return DesiredFrames(kMaxDelaySeconds, fpsNum, fpsDen, keyframeIntervalFrames);
}

double WindowBytes(const TopSegment *segments, size_t count, uint64_t nowNs, uint64_t windowFrames, uint64_t intervalNs,
		   double ratio)
{
	if (!segments || count == 0 || windowFrames == 0)
		return 0.0;
	if (ratio <= 0.0)
		ratio = 1.0;
	double bytes = 0.0;
	uint64_t remaining = windowFrames;
	uint64_t segmentEndNs = nowNs;
	// Newest segment first: the window is the newest windowFrames frames.
	for (size_t i = count; i-- > 0 && remaining > 0;) {
		const TopSegment &segment = segments[i];
		uint64_t frames = remaining; // The oldest segment takes whatever is left.
		if (i > 0 && intervalNs > 0) {
			const uint64_t spanNs = segmentEndNs > segment.fromNs ? segmentEndNs - segment.fromNs : 0;
			frames = std::min(remaining, (spanNs + intervalNs - 1) / intervalNs);
		}
		bytes += static_cast<double>(frames) *
			 (static_cast<double>(Nv12FrameBytes(segment.width, segment.height)) / ratio);
		remaining -= frames;
		segmentEndNs = segment.fromNs;
	}
	return bytes;
}

LiveChange ComputeLiveChange(const Input &in, const LiveRing &ring)
{
	LiveChange c;
	if (in.sourceWidth == 0 || in.sourceHeight == 0 || in.fpsNum == 0 || in.fpsDen == 0 || in.delaySeconds == 0 ||
	    ring.topWidth == 0 || ring.topHeight == 0 || ring.capacityFrames == 0)
		return c;
	c.valid = true;
	c.topWidth = ring.topWidth;
	c.topHeight = ring.topHeight;

	const double ratio = in.compressionRatio > 0.0 ? in.compressionRatio : 1.0;
	const double budget = static_cast<double>(VideoBudgetBytes(in));
	const uint64_t full = DesiredFrames(in.delaySeconds, in.fpsNum, in.fpsDen, in.keyframeIntervalFrames);
	const uint64_t desired = std::min(full, ring.capacityFrames);
	auto perFrame = [ratio](uint32_t width, uint32_t height) {
		return static_cast<double>(Nv12FrameBytes(width, height)) / ratio;
	};

	if (desired <= ring.windowFrames) {
		// Lowering (or no change): the window shrinks, nothing new to store.
		c.windowFrames = desired;
	} else {
		// Raising: every frame in the ring stays, and the extra slots are
		// captured at the top size -- or a smaller one -- from now on.
		const uint64_t extra = desired - ring.windowFrames;
		const double room = budget - ring.ringBytes;
		auto fitsAt = [&](uint32_t width, uint32_t height) {
			return room >= static_cast<double>(extra) * perFrame(width, height);
		};
		uint32_t width = ring.topWidth;
		uint32_t height = ring.topHeight;
		if (!fitsAt(width, height)) {
			// The largest size between the floor and the top that fits, walking
			// whole source rows down from the ideal one (Compute()'s walk).
			const double srcH = static_cast<double>(in.sourceHeight);
			const double capScale = OutputCapScale(in);
			const double floorScale =
				std::min(capScale, std::min(1.0, static_cast<double>(in.minResolutionHeight) / srcH));
			const auto floorRows = static_cast<uint32_t>(std::llround(floorScale * srcH));
			const auto topRows =
				static_cast<uint32_t>(std::floor(static_cast<double>(ring.topHeight) + 1e-6));
			uint32_t rows = topRows;
			if (room > 0.0) {
				const double fullResBytes = perFrame(in.sourceWidth, in.sourceHeight) > 0.0
								    ? perFrame(in.sourceWidth, in.sourceHeight)
								    : 1.0;
				const double ideal = std::sqrt(room / (static_cast<double>(extra) * fullResBytes));
				rows = std::min(rows, static_cast<uint32_t>(std::floor(ideal * srcH + 1e-6)));
			}
			rows = std::max(rows, floorRows);
			auto dimsAt = [&](uint32_t r, uint32_t &w, uint32_t &h) {
				const double s = static_cast<double>(r) / srcH;
				w = std::min(AlignedWidth(in.sourceWidth, s), ring.topWidth);
				h = std::min(EvenDim(in.sourceHeight, s), ring.topHeight);
			};
			dimsAt(rows, width, height);
			while (rows > floorRows && !fitsAt(width, height)) {
				--rows;
				dimsAt(rows, width, height);
			}
		}
		c.topWidth = width;
		c.topHeight = height;
		c.loweredTop = height < ring.topHeight || width < ring.topWidth;
		if (fitsAt(width, height)) {
			c.windowFrames = desired;
		} else {
			// Even the floor doesn't hold all of it: the whole seconds that do.
			const double each = perFrame(width, height);
			const uint64_t grow = room > 0.0 && each > 0.0 ? static_cast<uint64_t>(std::floor(room / each))
								       : 0;
			c.windowFrames = ring.windowFrames + std::min(grow, extra);
		}
	}
	c.effectiveDelaySeconds =
		EffectiveSecondsFor(c.windowFrames, in.delaySeconds, in.fpsNum, in.fpsDen, in.keyframeIntervalFrames);
	c.fitsFullDuration = c.windowFrames >= full;
	return c;
}

} // namespace trigglow::bufferfit
