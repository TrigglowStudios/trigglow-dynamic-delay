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

#include "loading-clip.hpp"

#include "logging.hpp"

#include <obs.h>
#include <media-io/video-io.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace trigglow {

namespace {

constexpr const char *kComponent = "loading-clip";
constexpr const char *kCaptureId = "trigglow_loading_clip_capture";
// An encoder that hasn't produced a whole loop by then isn't waited for.
constexpr int64_t kTimeoutNs = 25LL * 1000000000;

// What one capture output collects, written from the encoder's thread.
struct Capture {
	std::mutex m;
	int64_t loopMs = 8000;
	bool haveKey = false;
	int64_t firstDts = 0; // In the encoder's own time units, as are the frames' until Poll().
	double frameMs = 1000.0 / 30.0;
	bool done = false;
	int64_t loopEnd = 0; // Where the next loop's keyframe came.
	std::vector<relay::LoadingVideoTrack::Frame> frames;
	obs_output_t *output = nullptr;
};

void RegisterCaptureOutput()
{
	static std::once_flag once;
	std::call_once(once, [] {
		obs_output_info info = {};
		info.id = kCaptureId;
		info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
		info.encoded_video_codecs = "h264";
		info.get_name = [](void *) -> const char * {
			return "Trigglow loading clip";
		};
		info.create = [](obs_data_t *, obs_output_t *output) -> void * {
			auto *capture = new Capture();
			capture->output = output;
			return capture;
		};
		info.destroy = [](void *data) {
			delete static_cast<Capture *>(data);
		};
		info.start = [](void *data) -> bool {
			auto *capture = static_cast<Capture *>(data);
			if (!obs_output_can_begin_data_capture(capture->output, 0))
				return false;
			if (!obs_output_initialize_encoders(capture->output, 0))
				return false;
			return obs_output_begin_data_capture(capture->output, 0);
		};
		info.stop = [](void *data, uint64_t) {
			obs_output_end_data_capture(static_cast<Capture *>(data)->output);
		};
		info.encoded_packet = [](void *data, struct encoder_packet *packet) {
			auto *capture = static_cast<Capture *>(data);
			if (!packet || packet->type != OBS_ENCODER_VIDEO)
				return;
			std::lock_guard<std::mutex> lock(capture->m);
			if (capture->done)
				return;
			// Raw units here; Poll() turns them into milliseconds by frame
			// count (an encoder with a frame rate divisor counts its own way).
			const int64_t dts = packet->dts;
			const int64_t pts = packet->pts;
			if (!capture->haveKey) {
				if (!packet->keyframe)
					return;
				capture->haveKey = true;
				capture->firstDts = dts;
			} else if (packet->keyframe &&
				   static_cast<double>(capture->frames.size()) * capture->frameMs >=
					   static_cast<double>(capture->loopMs) - capture->frameMs / 2) {
				// The next loop starts here: the clip is whole GOPs.
				capture->done = true;
				capture->loopEnd = dts - capture->firstDts;
				return;
			}
			relay::LoadingVideoTrack::Frame frame;
			frame.data.assign(packet->data, packet->data + packet->size);
			frame.dtsMs = dts - capture->firstDts;
			frame.ptsMs = pts - capture->firstDts;
			frame.keyframe = packet->keyframe;
			capture->frames.push_back(std::move(frame));
		};
		obs_register_output(&info);
	});
}

} // namespace

struct LoadingClipMaker::Track {
	obs_canvas_t *canvas = nullptr;
	obs_scene_t *scene = nullptr;
	obs_encoder_t *encoder = nullptr;
	obs_output_t *output = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	double frameMs = 1000.0 / 30.0; // The copy's frame interval (frame rate and divisor).
	std::vector<uint8_t> header;
};

LoadingClipMaker::LoadingClipMaker() = default;

LoadingClipMaker::~LoadingClipMaker()
{
	Release();
}

bool LoadingClipMaker::Start(obs_output_t *output, const std::string &sceneName, int64_t loopMs, std::string &why,
			     const std::string &verticalVideo, const std::string &verticalStill)
{
	Release();
	state_ = State::Idle;
	loopMs_ = std::max<int64_t>(2000, loopMs);
	obs_source_t *loading = sceneName.empty() ? nullptr : obs_get_source_by_name(sceneName.c_str());
	if (!loading) {
		why = "the loading scene isn't there";
		return false;
	}
	obs_video_info main = {};
	if (!output || !obs_get_video_info(&main)) {
		obs_source_release(loading);
		why = "OBS has no video";
		return false;
	}
	RegisterCaptureOutput();
	for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; ++idx) {
		obs_encoder_t *source = obs_output_get_video_encoder2(output, idx);
		if (!source)
			break;
		const char *id = obs_encoder_get_id(source);
		if (!id || std::string(obs_encoder_get_codec(source) ? obs_encoder_get_codec(source) : "") != "h264") {
			why = "the stream isn't H.264";
			break;
		}
		auto track = std::make_unique<Track>();
		track->width = obs_encoder_get_width(source);
		track->height = obs_encoder_get_height(source);
		if (track->width == 0 || track->height == 0) {
			why = "an encoder has no size yet";
			break;
		}
		// The canvas: that encoder's size and frame rate.
		obs_video_info ovi = main;
		const uint32_t sourceDivisor = std::max<uint32_t>(1, obs_encoder_get_frame_rate_divisor(source));
		if (video_t *video = obs_encoder_video(source)) {
			if (const video_output_info *vi = video_output_get_info(video)) {
				ovi.fps_num = vi->fps_num;
				ovi.fps_den = vi->fps_den;
			}
		}
		if (ovi.fps_num > 0)
			track->frameMs = 1000.0 * ovi.fps_den * sourceDivisor / ovi.fps_num;
		ovi.base_width = ovi.output_width = track->width;
		ovi.base_height = ovi.output_height = track->height;
		const std::string name = "Trigglow loading clip " + std::to_string(idx);
		track->canvas = obs_canvas_create_private(name.c_str(), &ovi, ACTIVATE | EPHEMERAL);
		if (!track->canvas) {
			why = "couldn't make a canvas for the loading clip";
			tracks_.push_back(std::move(track));
			break;
		}
		// The loading scene fitted inside, centred (a vertical track gets a
		// chosen scene letterboxed instead of stretched).
		track->scene = obs_scene_create_private(name.c_str());
		auto fit = [&](obs_source_t *source) {
			obs_sceneitem_t *item = source ? obs_scene_add(track->scene, source) : nullptr;
			if (!item)
				return;
			obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			const vec2 bounds{static_cast<float>(track->width), static_cast<float>(track->height)};
			obs_sceneitem_set_bounds(item, &bounds);
			const vec2 pos{0.0f, 0.0f};
			obs_sceneitem_set_pos(item, &pos);
		};
		if (track->height > track->width && !verticalVideo.empty()) {
			// Trigglow's video made for vertical, its first frame under it
			// (the video takes a few frames to open: never a black one).
			if (!verticalVideo_) {
				if (!verticalStill.empty()) {
					obs_data_t *still = obs_data_create();
					obs_data_set_string(still, "file", verticalStill.c_str());
					verticalStill_ = obs_source_create_private(
						"image_source", "Trigglow loading clip vertical still", still);
					obs_data_release(still);
				}
				obs_data_t *video = obs_data_create();
				obs_data_set_bool(video, "is_local_file", true);
				obs_data_set_string(video, "local_file", verticalVideo.c_str());
				obs_data_set_bool(video, "looping", true);
				obs_data_set_bool(video, "restart_on_activate", true);
				verticalVideo_ = obs_source_create_private("ffmpeg_source",
									   "Trigglow loading clip vertical", video);
				obs_data_release(video);
			}
			fit(verticalStill_);
			fit(verticalVideo_ ? verticalVideo_ : loading);
		} else {
			fit(loading);
		}
		obs_canvas_set_channel(track->canvas, 0, obs_scene_get_source(track->scene));

		obs_data_t *settings = obs_encoder_get_settings(source);
		track->encoder = obs_video_encoder_create(id, name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (!track->encoder) {
			why = "couldn't copy the stream's encoder";
			tracks_.push_back(std::move(track));
			break;
		}
		obs_encoder_set_video(track->encoder, obs_canvas_get_video(track->canvas));
		const uint32_t divisor = obs_encoder_get_frame_rate_divisor(source);
		if (divisor > 1)
			obs_encoder_set_frame_rate_divisor(track->encoder, divisor);

		obs_data_t *outSettings = obs_data_create();
		track->output = obs_output_create(kCaptureId, name.c_str(), outSettings, nullptr);
		obs_data_release(outSettings);
		if (!track->output) {
			why = "couldn't make the capture output";
			tracks_.push_back(std::move(track));
			break;
		}
		if (auto *capture = static_cast<Capture *>(obs_obj_get_data(track->output))) {
			capture->loopMs = loopMs_;
			capture->frameMs = track->frameMs;
		}
		obs_output_set_video_encoder(track->output, track->encoder);
		tracks_.push_back(std::move(track));
	}
	obs_source_release(loading);
	if (!why.empty() || tracks_.empty()) {
		if (why.empty())
			why = "the stream has no video encoder";
		Release();
		return false;
	}
	for (auto &track : tracks_) {
		if (!obs_output_start(track->output)) {
			const char *error = obs_output_get_last_error(track->output);
			why = std::string("the loading clip's encoder didn't start") +
			      (error ? std::string(": ") + error : "");
			Release();
			return false;
		}
	}
	state_ = State::Running;
	startedNs_ = static_cast<int64_t>(os_gettime_ns());
	TRIGGLOW_LOG_INFO(kComponent, "making the loading clip: %zu track(s), %lld ms", tracks_.size(),
			  static_cast<long long>(loopMs_));
	return true;
}

LoadingClipMaker::State LoadingClipMaker::Poll(std::vector<relay::LoadingVideoTrack> &tracksOut, std::string &why)
{
	if (state_ != State::Running)
		return state_;
	bool allDone = true;
	for (auto &track : tracks_) {
		auto *capture = static_cast<Capture *>(obs_obj_get_data(track->output));
		std::lock_guard<std::mutex> lock(capture->m);
		allDone = allDone && capture->done;
	}
	if (!allDone) {
		if (static_cast<int64_t>(os_gettime_ns()) - startedNs_ > kTimeoutNs) {
			why = "the loading clip took too long";
			TRIGGLOW_LOG_WARN(kComponent, "%s; raising the delay replays instead", why.c_str());
			Release();
			state_ = State::Failed;
		}
		return state_;
	}
	tracksOut.clear();
	for (auto &track : tracks_) {
		relay::LoadingVideoTrack out;
		out.width = track->width;
		out.height = track->height;
		uint8_t *header = nullptr;
		size_t headerSize = 0;
		if (obs_encoder_get_extra_data(track->encoder, &header, &headerSize) && header && headerSize)
			out.header.assign(header, header + headerSize);
		auto *capture = static_cast<Capture *>(obs_obj_get_data(track->output));
		{
			std::lock_guard<std::mutex> lock(capture->m);
			out.frames = std::move(capture->frames);
		}
		int64_t loopEnd = 0;
		{
			std::lock_guard<std::mutex> lock(capture->m);
			loopEnd = capture->loopEnd;
		}
		// Encoder units -> milliseconds by frame count: the smallest step
		// between decode times is one frame.
		int64_t unit = 0;
		for (size_t i = 1; i < out.frames.size(); ++i) {
			const int64_t step = out.frames[i].dtsMs - out.frames[i - 1].dtsMs;
			if (step > 0 && (unit == 0 || step < unit))
				unit = step;
		}
		if (unit <= 0)
			unit = 1;
		auto toMs = [&](int64_t units) {
			return static_cast<int64_t>(std::llround(static_cast<double>(units) / unit * track->frameMs));
		};
		for (auto &frame : out.frames) {
			frame.dtsMs = toMs(frame.dtsMs);
			frame.ptsMs = toMs(frame.ptsMs);
		}
		out.loopMs = loopEnd > 0 ? toMs(loopEnd) : loopMs_;
		tracksOut.push_back(std::move(out));
	}
	TRIGGLOW_LOG_INFO(kComponent, "the loading clip is ready (%zu track(s))", tracksOut.size());
	Release();
	state_ = State::Done;
	return State::Done;
}

void LoadingClipMaker::Cancel()
{
	Release();
	state_ = State::Idle;
}

void LoadingClipMaker::Release()
{
	for (auto &track : tracks_) {
		if (track->output) {
			obs_output_force_stop(track->output);
			obs_output_release(track->output);
		}
		if (track->encoder)
			obs_encoder_release(track->encoder);
		if (track->canvas) {
			obs_canvas_set_channel(track->canvas, 0, nullptr);
			obs_canvas_remove(track->canvas);
			obs_canvas_release(track->canvas);
		}
		if (track->scene)
			obs_scene_release(track->scene);
	}
	tracks_.clear();
	if (verticalVideo_)
		obs_source_release(verticalVideo_);
	if (verticalStill_)
		obs_source_release(verticalStill_);
	verticalVideo_ = nullptr;
	verticalStill_ = nullptr;
}

} // namespace trigglow
