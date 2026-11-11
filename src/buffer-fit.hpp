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
#include <string_view>

// Pure sizing math for VideoDelayFilter's RAM ring: which resolution to buffer
// at and how many frames fit the RAM budget. Deliberately free of any libobs
// (or FFmpeg) dependency so tests/buffer_fit_tests.cpp can exercise it
// without OBS. VideoDelayFilter::EnsureRingSized() (actually resizes the ring)
// and VideoDelayFilter::EstimateBufferFit() (the dock's prediction) both call
// Compute() with the same Input, so what the dock predicts is what happens.
//
// Every width it returns is a multiple of kWidthAlignment (8), so each I420
// plane row is a multiple of 4 bytes (Y: width, U and V: width / 2). OpenGL
// needs that (v0.6.0): libobs-opengl's gs_stagesurface_map() and
// gs_texture_set_image() assume rows of exactly width bytes, while
// glReadPixels()/glTexSubImage2D() pad every row to GL_PACK/UNPACK_ALIGNMENT
// (4) -- any other plane width would shear the delayed picture on Linux and
// macOS. Heights only need to be even (one chroma row per two luma rows).
//
// Rules (v0.5.1, docs: buffer pipeline design A.1/A.6):
//   - Never buffer more pixels than OBS's main mix outputs. Anything above the
//     output resolution is thrown away by OBS's own output rescale anyway, but
//     it cost the plugin CPU (MJPEG encode/decode scale ~linearly with pixels).
//     The cap only applies when the target is canvas-sized (the normal live
//     scene case); anything else is left uncapped, the conservative choice.
//   - Quality floor second: never go below minResolutionHeight rows, unless
//     the output itself is smaller (storing more rows than OBS emits buys
//     nothing).
//   - Duration last: if the full delay doesn't fit the budget even at the
//     floor, the ring gets fewer frames instead.
//   - Exact frame rate (fpsNum/fpsDen), so 59.94 doesn't truncate to 59.
//   - Escape hatch: capToOutput = false (the hidden "buffer_output_cap"
//     plugin setting) drops the output cap and sizes like v0.5.0 did:
//     source resolution, only reduced for RAM, never below minResolutionHeight.
//     Not bit-identical to v0.5.0, though: the exact fps and the quarter
//     second of slack rounded up give one more slot at 25/30/50fps, when
//     RAM runs short the row walk below can land up to 2px away from it, and
//     since v0.6.0 widths are multiples of 8 (up to 7px narrower than the
//     source when it isn't).
namespace trigglow::bufferfit {

struct Input {
	uint32_t sourceWidth = 0; // obs_source_get_base_width/height(target)
	uint32_t sourceHeight = 0;
	uint32_t baseWidth = 0; // obs_video_info::base_* (canvas). 0 = unknown -> no output cap.
	uint32_t baseHeight = 0;
	uint32_t outputWidth = 0; // obs_video_info::output_*. 0 = unknown -> no output cap.
	uint32_t outputHeight = 0;
	uint32_t fpsNum = 0; // obs_video_info::fps_num / fps_den
	uint32_t fpsDen = 1;
	uint32_t delaySeconds = 0;
	uint32_t minResolutionHeight = 0;
	uint64_t budgetBytes = 0;
	double compressionRatio = 1.0; // Assumed bytes(raw NV12) / bytes(stored) per frame.
	// false = no output cap at all (OutputCapScale() returns 1.0). Support's
	// escape hatch for a case the cap gets wrong, see the header comment.
	bool capToOutput = true;
	// v0.7.0: the inter-frame storage codec's GOP, in frames (0 = intra,
	// every frame a key). The slack becomes at least one GOP, so the key of
	// the frame due always stays in the ring (invariant I4,
	// storage-codec.hpp).
	uint32_t keyframeIntervalFrames = 0;
	// v1.0.0: RAM the delay's AUDIO rings take per second of delay (raw PCM,
	// one ring per delayed audio source), taken out of budgetBytes before the
	// video ring is sized: delaySeconds x this. Nothing at 60 s (23 MB per
	// stereo source) but 230 MB per source at 600 s. 0 = none.
	uint64_t reservedBytesPerSecond = 0;
};

// v1.0.0: the longest delay scene mode takes (the dock, the controller and
// the ring's slot capacity), like stream mode's relay::kMaxDelaySeconds.
constexpr uint32_t kMaxDelaySeconds = 600;

// The video budget once the audio rings are taken out (reservedBytesPerSecond
// x delaySeconds), never below a quarter of budgetBytes.
uint64_t VideoBudgetBytes(const Input &in);

// v0.7.0 storage classes (fixed per delay cycle: invariant I9,
// storage-codec.hpp). The assumed ratio only decides how many seconds fit when
// RAM is short (the arena commits what the packets really weigh). Intra
// (MJPEG): 5, as since 2026-09-07 (real MJPEG runs at 15-20x). Inter (a
// hardware HEVC/H.264 cycle): 12, below what was measured even if that cycle
// falls back to MJPEG (HEVC 51x typical and 17x on the worst content, MJPEG
// 13-17x).
constexpr double kInterCompressionRatio = 12.0;
// The inter codec's keyframe interval: half a second of frames, ceil(fps / 2).
uint32_t KeyframeIntervalFrames(uint32_t fpsNum, uint32_t fpsDen);

// What a delay cycle's ring is sized for, fixed for the cycle:
//   Intra         MJPEG all the way: the intra ratio, a quarter second of slack.
//   IntraWithGop  MJPEG's ratio, but a GOP of slack: a hardware inter-frame
//                 codec may take over during the cycle -- the first one of a
//                 session, before any encoder passed a probe -- and a
//                 restarted decoder must still find its key in the ring.
//   Inter         a hardware codec worked this session: 12x, a GOP of slack.
enum class RingClass : uint8_t { Intra, IntraWithGop, Inter };
// "intra", "intra-gop", "inter": the filter's "storage_class" setting.
const char *RingClassName(RingClass ringClass);
// The reverse; anything else (missing, misspelled) is Intra, the conservative one.
RingClass ParseRingClass(std::string_view name);
// What the class sets in an Input: the ratio (intraRatio for the intra ones:
// 5 with FFmpeg, 1 without) and the GOP margin. EnsureRingSized() and
// EstimateBufferFit() both go through it, so the dock, the audio delay and the
// ring get the same whole-second delay.
void ApplyStorageClass(Input &in, RingClass ringClass, double intraRatio);

// Buffer widths are multiples of this (see the header comment).
constexpr uint32_t kWidthAlignment = 8;

struct Result {
	uint32_t bufferWidth = 0;     // A multiple of kWidthAlignment, >= 8. 0 only for invalid input.
	uint32_t bufferHeight = 0;    // Always even and >= 2 (I420 chroma subsampling).
	uint64_t actualFrames = 0;    // Ring slots that fit the budget (<= desiredFrames, >= 1).
	uint64_t desiredFrames = 0;   // Slots the full delay needs (delay*fps + slack + 1).
	double scale = 1.0;           // Final scale relative to the source.
	double capScale = 1.0;        // Ceiling from the OBS output resolution (1.0 = no cap).
	bool cappedByOutput = false;  // capScale < 1
	bool reducedByRam = false;    // scale < capScale
	bool fitsFullDuration = true; // actualFrames >= desiredFrames
	// v0.6.0 (design A.4): the delay video AND audio use, in whole seconds.
	// delaySeconds when the ring fits; otherwise the ring's span minus the
	// slack (a quarter second, or a GOP), floored to whole seconds (without the
	// slack if that would leave 0). Before, video used the fractional span and audio
	// its floor: up to ~1 s apart when RAM trimmed the ring.
	uint32_t effectiveDelaySeconds = 0;
};

Result Compute(const Input &in);

// Exposed for tests and for VideoDelayFilter's own bookkeeping.
double OutputCapScale(const Input &in);
// llround, clamp to [2, orig], then forced even. The floor of 2 (one NV12
// chroma sample) wins over the orig ceiling: orig 0 or 1 gives 2, never 0.
// For heights; widths use AlignedWidth().
uint32_t EvenDim(uint32_t orig, double scale);
// llround, clamp to orig, then the nearest multiple of kWidthAlignment that
// isn't above orig (rounded down when rounding up would pass it). The floor of
// kWidthAlignment wins over the orig ceiling, like EvenDim()'s floor of 2:
// a source narrower than 8 pixels gets an 8-pixel buffer (the capture stretches it).
uint32_t AlignedWidth(uint32_t orig, double scale);
uint64_t DesiredFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames = 0);
// The slack DesiredFrames() includes: a quarter second, ceil(fps / 4), or the
// GOP if that's longer (max(ceil(fps/4), GOP)); 0 for a 0s delay.
uint64_t SlackFrames(uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames = 0);
uint64_t FrameIntervalNs(uint32_t fpsNum, uint32_t fpsDen); // 1e9 * den / num; 0 if num == 0.
uint64_t Nv12FrameBytes(uint32_t width, uint32_t height);   // width * height * 3 / 2.

// The whole seconds a ring of `frames` slots delays by (Result::
// effectiveDelaySeconds): the span minus the slack, floored, never above
// delaySeconds; without the slack if that leaves 0.
uint32_t EffectiveSecondsFor(uint64_t frames, uint32_t delaySeconds, uint32_t fpsNum, uint32_t fpsDen,
			     uint32_t keyframeIntervalFrames);

// --- v1.0.0: changing the delay of a running ring in place ---
//
// The ring's slot headers are allocated once per cycle for kMaxDelaySeconds
// (CapacityFrames(), ~90 bytes each: 3 MB at 60fps), and a retention WINDOW
// says how many of them hold frames. A live delay change only moves the
// window and the playback target: lowering it skips forward at once (the
// frames past the new window are freed), raising it keeps every frame and
// the picture holds until the extra seconds have built up. Nothing is thrown
// away, so nothing refills from empty.
//
// Raising needs RAM for the extra seconds while every frame already in the
// ring stays. When they don't fit at the size captured now, the capture size
// (the degradation ladder's top) is lowered, never below the quality floor --
// the frames already stored keep their size and age out over the old delay;
// if even the floor doesn't fit, the delay is trimmed to the whole seconds
// that do (the dock shows "41 / 60 s", as at Enable). Neither the window nor
// the top ever goes back up within a cycle: the next Enable sizes afresh.

// Slots a ring needs to change its delay in place up to kMaxDelaySeconds.
uint64_t CapacityFrames(uint32_t fpsNum, uint32_t fpsDen, uint32_t keyframeIntervalFrames);

// One stretch of the running cycle captured at most at one size: from fromNs
// (capture time) until the next segment's fromNs, or now. A cycle starts
// with one segment at the ring's size; each live change that lowers the
// capture size adds one.
struct TopSegment {
	uint64_t fromNs = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

// Estimated bytes of the frames a window of windowFrames slots ending at nowNs
// holds (the newest windowFrames frames, one per intervalNs): each frame at
// its segment's size, divided by ratio. Frames older than the first segment
// count at its size. Conservative: frames the degradation ladder captured
// smaller still count at their segment's size.
double WindowBytes(const TopSegment *segments, size_t count, uint64_t nowNs, uint64_t windowFrames, uint64_t intervalNs,
		   double ratio);

struct LiveRing {
	uint64_t windowFrames = 0;   // Slots retained now.
	uint64_t capacityFrames = 0; // Slots the ring has (CapacityFrames()).
	uint32_t topWidth = 0;       // Largest size captured from now on (the ladder's top).
	uint32_t topHeight = 0;
	double ringBytes = 0.0; // WindowBytes() of the window now.
};

struct LiveChange {
	bool valid = false;        // False for invalid input: resize the ring instead.
	uint64_t windowFrames = 0; // The new retention window.
	uint32_t topWidth = 0;     // The ladder's top from now on (<= LiveRing's).
	uint32_t topHeight = 0;
	uint32_t effectiveDelaySeconds = 0; // What video and audio delay by from now on.
	bool fitsFullDuration = true;       // effectiveDelaySeconds == in.delaySeconds.
	bool loweredTop = false;            // The top went down so the extra seconds fit.
};

// The in-place change of a running ring to in.delaySeconds. `in` is the
// ring's own Input (geometry, fps, floor, budget, class) with the new delay.
LiveChange ComputeLiveChange(const Input &in, const LiveRing &ring);

} // namespace trigglow::bufferfit
