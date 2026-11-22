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

#include "vertical-taps.hpp"

#include <cstring>

extern "C" {
#include <graphics/vec2.h>
#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>
}

#include "logging.hpp"
#include "multistream-link.hpp"
#include "overlay-layout.hpp"

namespace trigglow {

namespace {

constexpr const char *kComponent = "vertical";
// Aitum Vertical's names (vertical-canvas.cpp): its stream outputs and its canvas.
constexpr const char *kOutputPrefix = "vertical_canvas_stream";
constexpr const char *kCanvasName = "Aitum Vertical";
// The same free output channel stream mode draws its badge on.
constexpr uint32_t kBadgeChannel = 61;
constexpr const char *kBadgeSourceName = "Trigglow Vertical Delay Badge (auto)";

bool StartsWith(const std::string &text, const char *prefix)
{
	return text.compare(0, std::strlen(prefix), prefix) == 0;
}

bool IsRtmpUrl(const std::string &url)
{
	return StartsWith(url, "rtmp://") || StartsWith(url, "rtmps://");
}

// The name Aitum's procs take: what follows "vertical_canvas_stream_" ("" for
// the unnamed output of its older single-output settings).
std::string AitumOutputName(const std::string &outputName)
{
	const size_t prefix = std::strlen(kOutputPrefix);
	return outputName.size() > prefix + 1 ? outputName.substr(prefix + 1) : std::string();
}

// "void aitum_vertical_<stop|start>_stream_output(in int width, in int height, in string name)";
// width and height 0 match Aitum's (first) vertical canvas.
bool CallAitum(const char *proc, const std::string &name)
{
	proc_handler_t *handler = obs_get_proc_handler();
	if (!handler)
		return false;
	calldata_t cd = {};
	calldata_set_int(&cd, "width", 0);
	calldata_set_int(&cd, "height", 0);
	calldata_set_string(&cd, "name", name.c_str());
	const bool called = proc_handler_call(handler, proc, &cd);
	calldata_free(&cd);
	return called;
}

void ServiceConnectInfo(obs_service_t *service, std::string &server, std::string &key)
{
	const char *s = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	const char *k = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : nullptr;
	server = s ? s : "";
	key = k ? k : "";
}

// The relay adds the host to its own messages ("Aitum Vertical (host): connected").
std::string DestinationName(const std::string &key)
{
	return mslink::IsTrigglowIngestKey(key) ? std::string("Aitum Vertical, ") + mslink::kTrigglowIngestName
						: std::string("Aitum Vertical");
}

} // namespace

struct VerticalTaps::Tap {
	std::string outputName;
	obs_weak_output_t *weak = nullptr;
	obs_service_t *original = nullptr; // Aitum's own service (a reference).
	obs_service_t *loop = nullptr;     // Ours, pointing at the relay.
	relay::StreamRelay relay;
	bool swapped = false;        // The output has our service.
	bool restartPending = false; // Stopped through Aitum after a direct start: start it again once taken.
	bool warnedUnsupported = false;
	std::string server; // What the relay sends to.
	std::string key;
};

VerticalTaps::VerticalTaps() = default;

VerticalTaps::~VerticalTaps()
{
	Shutdown();
}

void VerticalTaps::Update(bool enabled, const Options &options)
{
	options_ = options;

	struct Found {
		std::string name;
		obs_weak_output_t *weak;
	};
	std::vector<Found> found;
	// Under libobs' outputs mutex: plain reads and a weak reference only.
	obs_enum_outputs(
		[](void *param, obs_output_t *output) -> bool {
			const char *id = obs_output_get_id(output);
			const char *name = obs_output_get_name(output);
			if (id && name && std::strcmp(id, "rtmp_output") == 0 && StartsWith(name, kOutputPrefix))
				static_cast<std::vector<Found> *>(param)->push_back(
					{name, obs_output_get_weak_output(output)});
			return true;
		},
		&found);

	std::vector<Tap *> seen;
	for (Found &f : found) {
		obs_output_t *output = obs_weak_output_get_output(f.weak);
		obs_weak_output_release(f.weak);
		if (!output)
			continue;
		Tap *tap = nullptr;
		for (auto &t : taps_) {
			if (t->outputName != f.name)
				continue;
			obs_output_t *known = obs_weak_output_get_output(t->weak);
			const bool same = known == output;
			obs_output_release(known);
			if (same)
				tap = t.get();
		}
		if (!tap && enabled) {
			auto created = std::make_unique<Tap>();
			created->outputName = f.name;
			created->weak = obs_output_get_weak_output(output);
			created->relay.SetEventCallback([this](const std::string &message) {
				if (onEvent_)
					onEvent_(message);
			});
			tap = created.get();
			taps_.push_back(std::move(created));
		}
		if (tap) {
			Step(*tap, output, enabled);
			seen.push_back(tap);
		}
		obs_output_release(output);
	}

	// Outputs Aitum released (or recreated): their relays go.
	for (auto it = taps_.begin(); it != taps_.end();) {
		Tap *tap = it->get();
		bool keep = false;
		for (Tap *s : seen)
			keep = keep || s == tap;
		// Gone (what it was sending is cut), or handed back with stream mode off.
		if (!keep || (!enabled && !tap->swapped)) {
			ReleaseTap(*tap);
			it = taps_.erase(it);
		} else {
			++it;
		}
	}
}

void VerticalTaps::Step(Tap &tap, obs_output_t *output, bool enabled)
{
	const bool active = obs_output_active(output);
	if (!enabled) {
		GiveBack(tap, output);
		return;
	}

	if (!tap.swapped) {
		obs_service_t *service = obs_output_get_service(output);
		std::string server;
		std::string key;
		ServiceConnectInfo(service, server, key);
		if (!IsRtmpUrl(server) || key.empty()) {
			if (!tap.warnedUnsupported && !server.empty()) {
				tap.warnedUnsupported = true;
				TRIGGLOW_LOG_INFO(
					kComponent,
					"\"%s\" doesn't stream over RTMP or RTMPS: it stays as Aitum Vertical set it",
					tap.outputName.c_str());
			}
			return;
		}
		if (active) {
			// Created and started in the same call by Aitum: it went out directly.
			// Stopped through Aitum now (usually before its RTMP handshake is done),
			// taken while idle below, and started again.
			if (!tap.restartPending) {
				tap.restartPending = true;
				const bool stopped =
					CallAitum("aitum_vertical_stop_stream_output", AitumOutputName(tap.outputName));
				TRIGGLOW_LOG_INFO(
					kComponent,
					"\"%s\" started before the delay could take it: %s it to start it again through the delay",
					tap.outputName.c_str(), stopped ? "stopping" : "couldn't stop");
				if (!stopped)
					obs_output_stop(output);
			}
			return;
		}
		if (!tap.original)
			tap.original = obs_service_get_ref(service);
		if (!tap.original || !StartRelay(tap, server, key))
			return;
		obs_output_set_service(output, tap.loop);
		tap.swapped = true;
		TRIGGLOW_LOG_INFO(kComponent, "\"%s\" now goes through the delay (%s)", tap.outputName.c_str(),
				  DestinationName(key).c_str());
		if (tap.restartPending) {
			tap.restartPending = false;
			CallAitum("aitum_vertical_start_stream_output", AitumOutputName(tap.outputName));
		}
		return;
	}

	if (active)
		return; // Going out through the relay.

	// Idle: keep a fresh listener ready for Aitum's next start, at what its own
	// service says now (Aitum updates it before every start, from its settings).
	const relay::RelayStatus status = tap.relay.GetStatus();
	const bool listening = status.running && !status.ingestConnected && !status.ingestEnded;
	if (status.running && !listening && !tap.relay.Finished())
		return; // Still sending the seconds viewers haven't seen.
	std::string server;
	std::string key;
	ServiceConnectInfo(tap.original, server, key);
	if (listening && server == tap.server && key == tap.key)
		return;
	tap.relay.Stop();
	if (!IsRtmpUrl(server) || key.empty()) {
		GiveBack(tap, output);
		return;
	}
	StartRelay(tap, server, key);
}

bool VerticalTaps::StartRelay(Tap &tap, const std::string &server, const std::string &key)
{
	relay::Destination destination;
	destination.name = DestinationName(key);
	destination.server = server;
	destination.key = key;
	destination.primary = true;
	relay::RelayOptions options;
	options.delaySeconds = options_.delaySeconds;
	options.keepSeconds = options_.keepSeconds;
	options.drainOnStop = options_.drainOnStop;
	options.spillToDisk = options_.spillToDisk;
	std::string loopServer;
	std::string loopKey;
	std::string error;
	if (!tap.relay.Start({destination}, options, loopServer, loopKey, error)) {
		TRIGGLOW_LOG_ERROR(kComponent, "couldn't start the relay for \"%s\" (%s)", tap.outputName.c_str(),
				   error.c_str());
		if (onEvent_)
			onEvent_("Aitum Vertical: " + error);
		return false;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", loopServer.c_str());
	obs_data_set_string(settings, "key", loopKey.c_str());
	obs_data_set_bool(settings, "use_auth", false);
	if (tap.loop)
		obs_service_update(tap.loop, settings);
	else
		tap.loop = obs_service_create_private("rtmp_custom", "trigglow-dynamic-delay-vertical-relay", settings);
	obs_data_release(settings);
	tap.server = server;
	tap.key = key;
	return tap.loop != nullptr;
}

bool VerticalTaps::GiveBack(Tap &tap, obs_output_t *output)
{
	if (!tap.swapped) {
		tap.restartPending = false;
		return true;
	}
	if (obs_output_active(output))
		return false; // A live vertical stream keeps the delay until it stops.
	const relay::RelayStatus status = tap.relay.GetStatus();
	if (status.running && status.ingestConnected && !tap.relay.Finished())
		return false; // Its last seconds are still being sent.
	tap.relay.Stop();
	if (tap.original)
		obs_output_set_service(output, tap.original);
	tap.swapped = false;
	TRIGGLOW_LOG_INFO(kComponent, "\"%s\" goes straight to its platform again", tap.outputName.c_str());
	return true;
}

void VerticalTaps::ReleaseTap(Tap &tap)
{
	tap.relay.Stop();
	if (tap.swapped && tap.original) {
		if (obs_output_t *output = obs_weak_output_get_output(tap.weak)) {
			obs_output_set_service(output, tap.original);
			obs_output_release(output);
		}
	}
	tap.swapped = false;
	obs_service_release(tap.loop);
	tap.loop = nullptr;
	obs_service_release(tap.original);
	tap.original = nullptr;
	obs_weak_output_release(tap.weak);
	tap.weak = nullptr;
}

void VerticalTaps::SetDelay(uint32_t seconds)
{
	options_.delaySeconds = seconds;
	for (auto &tap : taps_) {
		if (tap->swapped && tap->relay.Running())
			tap->relay.SetDelay(seconds);
	}
}

void VerticalTaps::SetKeepSeconds(uint32_t seconds)
{
	options_.keepSeconds = seconds;
	for (auto &tap : taps_) {
		if (tap->swapped && tap->relay.Running())
			tap->relay.SetKeepSeconds(seconds);
	}
}

void VerticalTaps::Dump()
{
	for (auto &tap : taps_) {
		if (tap->swapped && tap->relay.Running() && tap->relay.GetStatus().ingestConnected)
			tap->relay.Dump();
	}
}

void VerticalTaps::Shutdown()
{
	ClearBadge();
	for (auto &tap : taps_)
		ReleaseTap(*tap);
	taps_.clear();
}

bool VerticalTaps::Owns(const obs_output_t *output) const
{
	obs_service_t *service = obs_output_get_service(output);
	if (!service)
		return false;
	for (const auto &tap : taps_) {
		if (tap->swapped && tap->loop == service)
			return true;
	}
	return false;
}

bool VerticalTaps::AnyStreaming() const
{
	for (const auto &tap : taps_) {
		if (!tap->swapped || !tap->relay.Running())
			continue;
		const relay::RelayStatus status = tap->relay.GetStatus();
		if (status.ingestConnected || (status.ingestEnded && !tap->relay.Finished()))
			return true;
	}
	return false;
}

std::vector<relay::SinkStatus> VerticalTaps::Sinks() const
{
	std::vector<relay::SinkStatus> sinks;
	for (const auto &tap : taps_) {
		if (!tap->swapped || !tap->relay.Running())
			continue;
		const relay::RelayStatus status = tap->relay.GetStatus();
		if (!status.ingestConnected && !status.ingestEnded)
			continue;
		for (relay::SinkStatus sink : status.sinks) {
			sink.primary = false;
			sinks.push_back(std::move(sink));
		}
	}
	return sinks;
}

void VerticalTaps::SetBadge(bool visible, const std::string &imagePath, uint32_t corner, bool canvasDelayed)
{
	bool tapped = canvasDelayed;
	for (const auto &tap : taps_)
		tapped = tapped || tap->swapped;
	obs_canvas_t *canvas = obs_get_canvas_by_name(kCanvasName);
	if (!canvas || !visible || !tapped || imagePath.empty()) {
		if (canvas && badgeShown_)
			obs_canvas_set_channel(canvas, kBadgeChannel, nullptr);
		badgeShown_ = false;
		obs_canvas_release(canvas);
		return;
	}
	obs_video_info ovi = {};
	if (!obs_canvas_get_video_info(canvas, &ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
		obs_canvas_release(canvas);
		return;
	}
	if (badgeShown_ && imagePath == badgePath_ && corner == badgeCorner_ && ovi.base_width == badgeWidth_ &&
	    ovi.base_height == badgeHeight_) {
		obs_canvas_release(canvas);
		return;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", imagePath.c_str());
	if (!badgeSource_)
		badgeSource_ = obs_source_create_private("image_source", kBadgeSourceName, settings);
	else
		obs_source_update(badgeSource_, settings);
	obs_data_release(settings);
	if (!badgeScene_) {
		badgeScene_ = obs_scene_create_private("Trigglow Vertical Delay Overlay (auto)");
		obs_scene_add(badgeScene_, badgeSource_);
	}
	const overlaylayout::Placement p =
		overlaylayout::Place(static_cast<float>(ovi.base_width), static_cast<float>(ovi.base_height), corner);
	if (obs_sceneitem_t *item = obs_scene_find_source(badgeScene_, kBadgeSourceName)) {
		const vec2 pos{p.x, p.y};
		const vec2 scale{p.scale, p.scale};
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_scale(item, &scale);
	}
	obs_canvas_set_channel(canvas, kBadgeChannel, obs_scene_get_source(badgeScene_));
	obs_canvas_release(canvas);
	badgeShown_ = true;
	badgePath_ = imagePath;
	badgeCorner_ = corner;
	badgeWidth_ = ovi.base_width;
	badgeHeight_ = ovi.base_height;
}

void VerticalTaps::ClearBadge()
{
	if (badgeShown_) {
		if (obs_canvas_t *canvas = obs_get_canvas_by_name(kCanvasName)) {
			obs_canvas_set_channel(canvas, kBadgeChannel, nullptr);
			obs_canvas_release(canvas);
		}
	}
	badgeShown_ = false;
	if (badgeScene_) {
		obs_scene_release(badgeScene_);
		badgeScene_ = nullptr;
	}
	if (badgeSource_) {
		obs_source_release(badgeSource_);
		badgeSource_ = nullptr;
	}
}

} // namespace trigglow
