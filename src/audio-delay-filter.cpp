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

#include "audio-delay-filter.hpp"
#include "i18n.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cstring>

namespace trigglow {

namespace {
constexpr const char *kComponent = "audio-delay-filter";
constexpr const char *kFilterId = "trigglow_audio_delay_filter";
constexpr const char *kSettingDelaySeconds = "delay_seconds";
// The one entry of the filter's properties: a note that the dock manages it.
// Never a stored setting.
constexpr const char *kPropertyManagedInfo = "trigglow_managed_info";

// OBS hands a filter exactly contiguous timestamps from a source that is
// delivering steadily, so anything past this is a real gap (or rewind), not
// jitter. Small enough that dropouts don't accumulate into audible A/V drift.
constexpr uint64_t kGapToleranceNs = 10ULL * 1000 * 1000;

// Bumped by the bridge on every Enable to flush stale ring contents.
constexpr const char *kSettingResetToken = "reset_token";
} // namespace

const char *AudioDelayFilter::Id()
{
	return kFilterId;
}

void AudioDelayFilter::Register()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = &AudioDelayFilter::GetName;
	info.create = &AudioDelayFilter::Create;
	info.destroy = &AudioDelayFilter::Destroy;
	info.update = &AudioDelayFilter::UpdateCb;
	info.filter_audio = &AudioDelayFilter::FilterAudioCb;
	info.get_properties = &AudioDelayFilter::GetProperties;
	info.get_defaults = &AudioDelayFilter::GetDefaults;
	obs_register_source(&info);
}

AudioDelayFilter::AudioDelayFilter(obs_source_t *filterSource) : filterSource_(filterSource) {}

obs_audio_data *AudioDelayFilter::FilterAudio(obs_audio_data *audio)
{
	if (!audio || audio->frames == 0)
		return audio;

	audio_t *audioContext = obs_get_audio();
	auto channels = static_cast<uint32_t>(audio_output_get_channels(audioContext));
	uint32_t sampleRate = audio_output_get_sample_rate(audioContext);
	if (channels == 0 || sampleRate == 0)
		return audio;

	// One consistent snapshot for the whole call: Update() can change the
	// delay from the UI thread at any moment.
	const uint32_t delaySeconds = configuredDelaySeconds_.load(std::memory_order_relaxed);

	line_.Configure(channels, sampleRate);
	if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
		line_.Reset();
		// A new cycle: the source's timeline starts over too, so the time the
		// filter was off (Disable to Enable) isn't taken for a gap.
		haveExpectedTimestamp_ = false;
	}
	// Lowered: skips forward at once; raised: holds (silence) until the extra
	// seconds have built up -- what the video does (AudioDelayLine).
	line_.SetDelayFrames(static_cast<uint64_t>(delaySeconds) * sampleRate);
	const size_t frames = audio->frames;

	// --- Keep the line on the wall clock, not on "samples received" ---
	// Sources that stop delivering audio while idle (application/window
	// audio capture, paused media, browser sources) would otherwise make
	// this delay stretch beyond the video's: the line only advanced when
	// samples arrived. A forward gap is filled with silence; a rewind, or a
	// gap longer than the whole ring (source restarted), invalidates the
	// history instead.
	const auto chunkNs = static_cast<uint64_t>(frames) * 1000000000ULL / sampleRate;
	if (haveExpectedTimestamp_) {
		auto drift = static_cast<int64_t>(audio->timestamp) - static_cast<int64_t>(expectedTimestamp_);
		if (drift > static_cast<int64_t>(kGapToleranceNs)) {
			const uint64_t gapFrames = static_cast<uint64_t>(drift) * sampleRate / 1000000000ULL;
			const uint64_t room = line_.TargetFrames() +
					      static_cast<uint64_t>(AudioDelayLine::kMarginSeconds) * sampleRate;
			const bool tooLong = gapFrames >= room;
			if (tooLong)
				line_.Reset();
			else
				line_.WriteSilence(static_cast<size_t>(gapFrames));
			if (timelineEventCount_++ < 20)
				TRIGGLOW_LOG_INFO(kComponent, "\"%s\": %lldms gap in source audio, %s",
						  obs_source_get_name(filterSource_),
						  static_cast<long long>(drift / 1000000LL),
						  tooLong ? "history reset" : "filled with silence");
		} else if (drift < -static_cast<int64_t>(kGapToleranceNs)) {
			line_.Reset();
			if (timelineEventCount_++ < 20)
				TRIGGLOW_LOG_INFO(kComponent,
						  "\"%s\": source timestamp went back %lldms, history reset",
						  obs_source_get_name(filterSource_),
						  static_cast<long long>(-drift / 1000000LL));
		}
	}
	haveExpectedTimestamp_ = true;
	expectedTimestamp_ = audio->timestamp + chunkNs;

	line_.WriteGated(reinterpret_cast<const float *const *>(audio->data), frames,
			 gateOpen_.load(std::memory_order_relaxed));

	// --- Read: the chunk that is delaySeconds old (silence while filling) ---
	// Program is on the loading scene while it fills, so that silence is never
	// actually heard there.
	outputChunks_.resize(channels);
	float *out[MAX_AV_PLANES] = {};
	for (uint32_t c = 0; c < channels && c < MAX_AV_PLANES; ++c) {
		outputChunks_[c].resize(frames);
		out[c] = outputChunks_[c].data();
	}
	line_.Read(out, frames);

	for (uint32_t c = 0; c < MAX_AV_PLANES; ++c)
		output_.data[c] = c < channels ? reinterpret_cast<uint8_t *>(outputChunks_[c].data()) : nullptr;
	output_.frames = static_cast<uint32_t>(frames);
	// Passthrough, not recomputed: mirrors VideoDelayFilter, which draws old
	// pixels at the CURRENT instant rather than rewinding a timestamp.
	output_.timestamp = audio->timestamp;

	return &output_;
}

void AudioDelayFilter::ApplyDelaySeconds(uint32_t seconds, bool fromCollection)
{
	if (seconds == configuredDelaySeconds_.load())
		return;
	configuredDelaySeconds_.store(seconds);
	// v1.0.0: the value a filter loads with is only the last one it was given
	// (one live scene at 60 s, another later at 30 s leave both values in the
	// collection), and the filter loads switched off: the dock gives every
	// filter of the live scene its delay before switching them on.
	if (fromCollection)
		TRIGGLOW_LOG_INFO(kComponent,
				  "\"%s\" loaded with the %us it last had (off; the dock sets the delay when it "
				  "turns the delay on)",
				  obs_source_get_name(filterSource_), seconds);
	else
		TRIGGLOW_LOG_INFO(kComponent, "delay set to %us on \"%s\"", seconds,
				  obs_source_get_name(filterSource_));
}

void AudioDelayFilter::Update(obs_data_t *settings)
{
	{
		// The dock's delay once it has pushed one (SetManagedDelaySecondsProc):
		// any other value in the settings came from elsewhere and is ignored.
		std::lock_guard<std::mutex> lock(managedMutex_);
		const auto seconds = static_cast<uint32_t>(obs_data_get_int(settings, kSettingDelaySeconds));
		if (!managed_) {
			ApplyDelaySeconds(seconds, !created_);
		} else if (seconds != configuredDelaySeconds_.load() && !warnedForeignSettings_) {
			warnedForeignSettings_ = true;
			TRIGGLOW_LOG_WARN(kComponent,
					  "\"%s\": delay_seconds=%u set outside the Trigglow dock -- ignored, the dock "
					  "manages this filter (%us)",
					  obs_source_get_name(filterSource_), seconds, configuredDelaySeconds_.load());
		}
	}

	int64_t token = obs_data_get_int(settings, kSettingResetToken);
	if (token != lastResetToken_) {
		lastResetToken_ = token;
		resetRequested_.store(true, std::memory_order_release);
	}
}

const char *AudioDelayFilter::GetName(void * /*typeData*/)
{
	return Str("Filter.AudioDelay.Name");
}

void *AudioDelayFilter::Create(obs_data_t *settings, obs_source_t *source)
{
	auto *filter = new AudioDelayFilter(source);
	filter->Update(settings);
	filter->created_ = true;
	if (proc_handler_t *procHandler = obs_source_get_proc_handler(source)) {
		proc_handler_add(procHandler, "void set_managed_delay_seconds(in int seconds)",
				 &AudioDelayFilter::SetManagedDelaySecondsProc, filter);
		proc_handler_add(procHandler, "void set_gate_open(in bool open)", &AudioDelayFilter::SetGateOpenProc,
				 filter);
	}
	return filter;
}

void AudioDelayFilter::Destroy(void *data)
{
	delete static_cast<AudioDelayFilter *>(data);
}

void AudioDelayFilter::SetManagedDelaySecondsProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<AudioDelayFilter *>(data);
	const auto seconds =
		static_cast<uint32_t>(std::clamp<long long>(calldata_int(params, "seconds"), 0, UINT32_MAX));
	std::lock_guard<std::mutex> lock(filter->managedMutex_);
	filter->managed_ = true;
	filter->warnedForeignSettings_ = false; // One WARN per push of the dock's value, at most.
	filter->ApplyDelaySeconds(seconds, false);
	// The settings say the same (saved with the collection, compared with by
	// Update()), under the mutex like VideoDelayFilter::SetManagedSetting().
	if (obs_data_t *settings = obs_source_get_settings(filter->filterSource_)) {
		obs_data_set_int(settings, kSettingDelaySeconds, seconds);
		obs_data_release(settings);
	}
}

void AudioDelayFilter::SetGateOpenProc(void *data, calldata_t *params)
{
	auto *filter = static_cast<AudioDelayFilter *>(data);
	const bool open = calldata_bool(params, "open");
	if (filter->gateOpen_.exchange(open, std::memory_order_relaxed) != open)
		TRIGGLOW_LOG_INFO(
			kComponent, "\"%s\": %s", obs_source_get_name(filter->filterSource_),
			open ? "gate open: heard (in the delay, its scene's sound comes out one delay from now)"
			     : "gate closed: what it already holds plays out, silent after that");
}

void AudioDelayFilter::UpdateCb(void *data, obs_data_t *settings)
{
	static_cast<AudioDelayFilter *>(data)->Update(settings);
}

obs_audio_data *AudioDelayFilter::FilterAudioCb(void *data, obs_audio_data *audio)
{
	return static_cast<AudioDelayFilter *>(data)->FilterAudio(audio);
}

obs_properties_t *AudioDelayFilter::GetProperties(void * /*data*/)
{
	// No delay field: the dock manages this filter and keeps it on the video's
	// delay (a value set here put audio out of step with the video, and
	// Update() ignores it anyway once the dock has pushed its own).
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, kPropertyManagedInfo, Str("Filter.AudioDelay.Managed"), OBS_TEXT_INFO);
	return props;
}

void AudioDelayFilter::GetDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kSettingDelaySeconds, 0);
	obs_data_set_default_int(settings, kSettingResetToken, 0);
}

} // namespace trigglow
