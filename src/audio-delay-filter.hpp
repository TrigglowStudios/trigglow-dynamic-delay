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

#include "audio-delay-line.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

extern "C" {
#include <obs.h>
#include <media-io/audio-io.h> // audio_output_get_channels/sample_rate()
}

// PHASE 2 (audio sync) -- companion to VideoDelayFilter, but attached
// differently. VideoDelayFilter attaches to the live SCENE and can only
// carry video, because libobs's obs_source_filter_add() silently refuses
// any filter requesting OBS_SOURCE_AUDIO on a source whose own
// output_flags don't include it -- and scenes never advertise audio in
// output_flags (their audio goes through a separate .audio_render
// callback, see obs-scene.c). Found live, 2026-08-25, after several failed
// attempts to make a combined video+audio filter attach to a scene at all.
//
// This filter instead attaches to each individual LEAF audio source INSIDE
// the live scene (Mic, Desktop, etc. -- real wasapi_input_capture /
// wasapi_output_capture sources, which DO advertise OBS_SOURCE_AUDIO on
// themselves, so obs_source_filter_add() accepts it normally). Verified
// live, 2026-08-25, that a leaf source's filter_audio callback keeps firing
// on schedule even while that source isn't part of the current Program
// scene's tree -- unlike the scene-level video/audio mixing tree, a leaf
// source's own obs_source_output_audio() -> filter chain runs continuously
// regardless of visibility, so this filter (unlike VideoDelayFilter) needs
// NO keep-alive trick at all. One instance is attached per audio-capable
// child of the live scene; ObsFrontendBridge manages enabling/disabling/
// configuring all of them together so they always agree with the video
// delay's seconds and Filling/Active state.
//
// The delay itself is AudioDelayLine (v1.0.0; before, the same ring logic
// lived here and every delay change emptied it). Raw PCM, once per attached
// leaf source: 23 MB per stereo source at 60 s, 230 MB at 600 s, which the
// video ring's RAM budget sets aside since v1.0.0
// (bufferfit::Input::reservedBytesPerSecond).
namespace trigglow {

class AudioDelayFilter {
public:
	static const char *Id();

	// Registers this filter type with OBS. Call once from obs_module_load().
	static void Register();

private:
	explicit AudioDelayFilter(obs_source_t *filterSource);
	~AudioDelayFilter() = default;

	void Update(obs_data_t *settings);
	obs_audio_data *FilterAudio(obs_audio_data *audio);
	// Sets configuredDelaySeconds_, logging a real change. Under managedMutex_.
	// fromCollection: the value saved with the scene collection, at creation.
	void ApplyDelaySeconds(uint32_t seconds, bool fromCollection);

	// --- obs_source_info callback trampolines ---
	static const char *GetName(void *typeData);
	static void *Create(obs_data_t *settings, obs_source_t *source);
	static void Destroy(void *data);
	static void UpdateCb(void *data, obs_data_t *settings);
	static obs_audio_data *FilterAudioCb(void *data, obs_audio_data *audio);
	static obs_properties_t *GetProperties(void *data);
	static void GetDefaults(obs_data_t *settings);

	// "set_managed_delay_seconds(in int seconds)" proc handler: how
	// ObsFrontendBridge pushes the delay audio must use (the video's
	// effective one). Like VideoDelayFilter's: from then on Update() ignores
	// any other delay_seconds in the settings (one WARN per push), so OBS's
	// Defaults button or obs-websocket can't put audio out of step with the
	// video. Applies on the caller's thread.
	static void SetManagedDelaySecondsProc(void *data, calldata_t *params);

	obs_source_t *filterSource_; // Not owned; valid for this object's lifetime.
	// Written by Update() (UI thread), read by FilterAudio() (audio thread).
	std::atomic<uint32_t> configuredDelaySeconds_{0};
	// See SetManagedDelaySecondsProc(). managedMutex_ guards both flags and
	// keeps the proc's value and its write into the settings together
	// against Update().
	std::mutex managedMutex_;
	bool managed_ = false;
	bool warnedForeignSettings_ = false;
	bool created_ = false; // Create() has run: Update() calls after it aren't the collection's load.
	// Bumped by Update() whenever the "reset_token" setting changes (the
	// bridge does this on every Enable); consumed by the audio thread.
	std::atomic<bool> resetRequested_{false};
	int64_t lastResetToken_ = 0;

	// Timestamp (ns) at which the next chunk is expected to start if the
	// source is delivering contiguously. Gaps/rewinds vs. this are how we
	// notice a source that went quiet (e.g. application capture that sends
	// nothing while the app is silent) or restarted.
	bool haveExpectedTimestamp_ = false;
	uint64_t expectedTimestamp_ = 0;
	uint32_t timelineEventCount_ = 0; // Rate-limits gap/rewind log lines.

	// v1.0.0: the delay line itself (audio-delay-line.hpp): keeps its history
	// across a delay change, so a live change skips forward or holds in step
	// with the video instead of emptying the ring.
	AudioDelayLine line_;

	// Owned output storage for FilterAudio(): filter_audio's contract
	// requires returned data to stay valid until the next call, so this
	// can't be a stack buffer -- resized to match each call's audio->frames.
	std::vector<std::vector<float>> outputChunks_;
	obs_audio_data output_ = {};
};

} // namespace trigglow
