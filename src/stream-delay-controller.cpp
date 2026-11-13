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

#include "stream-delay-controller.hpp"

#include <QTimer>

#include <algorithm>
#include <cstring>

extern "C" {
#include <graphics/vec2.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>
}

#include "logging.hpp"
#include "overlay-layout.hpp"
#include "packet-delay-core.hpp"
#include "spill-file.hpp"

namespace trigglow {

namespace {

constexpr const char *kComponent = "stream-mode";

void EventTrampoline(enum obs_frontend_event event, void *param)
{
	StreamDelayController::FrontendEvent(static_cast<int>(event), param);
}

bool StartsWith(const char *text, const char *prefix)
{
	return text && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

std::string ServiceDisplayName(obs_service_t *service, const std::string &server)
{
	obs_data_t *settings = obs_service_get_settings(service);
	std::string name = settings ? obs_data_get_string(settings, "service") : "";
	obs_data_release(settings);
	if (name.empty())
		name = relay::HostOf(server);
	return name;
}

} // namespace

StreamDelayController::StreamDelayController()
{
	relay_.SetEventCallback([this](const std::string &message) {
		std::lock_guard<std::mutex> lock(eventMutex_);
		lastEvent_ = message;
	});
}

StreamDelayController::~StreamDelayController()
{
	Shutdown();
}

void StreamDelayController::Init()
{
	if (initialized_)
		return;
	initialized_ = true;
	obs_frontend_add_event_callback(EventTrampoline, this);
	timer_ = std::make_unique<QTimer>();
	timer_->setInterval(500);
	QObject::connect(timer_.get(), &QTimer::timeout, [this] { Tick(); });
	timer_->start();
	// For the dock's "Automatic (Memory, SSD detected)" before any stream; a
	// few storage queries, no I/O on the drive itself.
	driveKind_ = relay::DetectDriveKind(relay::SpillFile::TempDirectory());
	TRIGGLOW_LOG_INFO(kComponent, "stream mode %s (%s)", relay::RelayAvailable() ? "available" : "unavailable",
			  applyTo_ == ApplyTo::Stream ? "on" : "off");
}

void StreamDelayController::Shutdown()
{
	if (!initialized_)
		return;
	initialized_ = false;
	obs_frontend_remove_event_callback(EventTrampoline, this);
	if (timer_)
		timer_->stop();
	timer_.reset();
	RestoreFrontendService();
	relay_.Stop();
	if (loopbackService_) {
		obs_service_release(loopbackService_);
		loopbackService_ = nullptr;
	}
	engaged_ = false;
	ClearOverlay();
}

bool StreamDelayController::Dump()
{
	if (!engaged_ || !delayOn_ || relay_.Delay() == 0)
		return false;
	relay_.Dump();
	// Back to the delay once there are its seconds of stream after the dump
	// (plus a keyframe interval for the splice), rewinding only into them.
	dumpRestoreAtNs_ =
		static_cast<int64_t>(os_gettime_ns()) + (static_cast<int64_t>(delaySeconds_) + 3) * 1000000000LL;
	TRIGGLOW_LOG_INFO(kComponent, "dump: unaired seconds dropped; the %us delay comes back on its own",
			  delaySeconds_);
	Notify();
	return true;
}

void StreamDelayController::SetPreset(int index, uint32_t seconds)
{
	if (index < 0 || index > 2)
		return;
	presets_[index] = std::max<uint32_t>(1, relay::ClampDelaySeconds(seconds));
	if (engaged_)
		relay_.SetKeepSeconds(RamKeepSeconds());
	Notify();
}

uint32_t StreamDelayController::RamKeepSeconds() const
{
	uint32_t seconds = 0;
	for (uint32_t preset : presets_)
		seconds = std::max(seconds, preset);
	for (const SceneRule &rule : sceneRules_) {
		if (rule.action == SceneAction::DelayOn)
			seconds = std::max(seconds, rule.seconds);
	}
	return relay::ClampDelaySeconds(seconds);
}

uint32_t StreamDelayController::Preset(int index) const
{
	return (index >= 0 && index <= 2) ? presets_[index] : 0;
}

void StreamDelayController::ApplyPreset(int index)
{
	if (index < 0 || index > 2)
		return;
	SetDelaySeconds(presets_[index]);
	Enable();
}

void StreamDelayController::SetSceneRules(std::vector<SceneRule> rules)
{
	sceneRules_ = std::move(rules);
	if (engaged_)
		relay_.SetKeepSeconds(RamKeepSeconds());
	Notify();
}

void StreamDelayController::ApplySceneRule()
{
	if (!HandlesControls())
		return;
	obs_source_t *scene = obs_frontend_get_current_scene();
	const std::string name = scene ? obs_source_get_name(scene) : "";
	obs_source_release(scene);
	for (const SceneRule &rule : sceneRules_) {
		if (rule.scene != name || rule.action == SceneAction::None)
			continue;
		ruleScene_ = name;
		if (rule.action == SceneAction::DelayOn) {
			if (rule.seconds > 0)
				SetDelaySeconds(rule.seconds);
			if (!delayOn_)
				TRIGGLOW_LOG_INFO(kComponent, "scene rule: delay on for this scene");
			Enable();
		} else {
			if (delayOn_)
				TRIGGLOW_LOG_INFO(kComponent, "scene rule: delay off for this scene");
			Disable();
		}
		return;
	}
}

void StreamDelayController::SetShowOverlay(bool show)
{
	showOverlay_ = show;
	UpdateOverlay();
	Notify();
}

void StreamDelayController::SetOverlayShowSeconds(bool show)
{
	if (overlayShowSeconds_ == show)
		return;
	overlayShowSeconds_ = show;
	overlaySeconds_ = 0; // Redraw it with or without the seconds.
	UpdateOverlay();
	Notify();
}

void StreamDelayController::SetOverlayCorner(uint32_t corner)
{
	overlayCorner_ = corner > 3 ? 0 : corner;
	overlaySeconds_ = 0; // Re-place it.
	UpdateOverlay();
	Notify();
}

// The badge goes on a libobs output channel, which draws over the main view
// (program) after the scene: the stream, so viewers get it with the delayed
// picture, and the recording too. Channels 0-6 are OBS's own (transition,
// global audio); downstream-keyer style plugins use the low ones. Shown from
// the moment the delay is on, also armed before the stream starts, so the
// streamer sees it in OBS right away, as in scene mode. Armed only counts
// while this mode is the selected one (all of OBS draws its own badge).
void StreamDelayController::UpdateOverlay()
{
	constexpr uint32_t kOverlayChannel = 61;
	const bool want = showOverlay_ && delayOn_ && overlayImage_ && (engaged_ || HandlesControls());
	if (!want) {
		if (overlayShown_) {
			obs_set_output_source(kOverlayChannel, nullptr);
			overlayShown_ = false;
		}
		return;
	}
	if (overlayShown_ && overlaySeconds_ == delaySeconds_)
		return;
	const std::string path = overlayImage_(delaySeconds_);
	if (path.empty())
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "file", path.c_str());
	if (!overlaySource_)
		overlaySource_ =
			obs_source_create_private("image_source", "Trigglow Stream Delay Badge (auto)", settings);
	else
		obs_source_update(overlaySource_, settings);
	obs_data_release(settings);
	if (!overlayScene_) {
		overlayScene_ = obs_scene_create_private("Trigglow Stream Delay Overlay (auto)");
		obs_scene_add(overlayScene_, overlaySource_);
	}

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const overlaylayout::Placement p = overlaylayout::Place(static_cast<float>(ovi.base_width),
								static_cast<float>(ovi.base_height), overlayCorner_);
	if (obs_sceneitem_t *item = obs_scene_find_source(overlayScene_, "Trigglow Stream Delay Badge (auto)")) {
		const vec2 pos{p.x, p.y};
		const vec2 scale{p.scale, p.scale};
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_scale(item, &scale);
	}
	obs_set_output_source(kOverlayChannel, obs_scene_get_source(overlayScene_));
	overlayShown_ = true;
	overlaySeconds_ = delaySeconds_;
}

void StreamDelayController::ClearOverlay()
{
	if (overlayShown_)
		obs_set_output_source(61, nullptr);
	overlayShown_ = false;
	if (overlayScene_) {
		obs_scene_release(overlayScene_);
		overlayScene_ = nullptr;
	}
	if (overlaySource_) {
		obs_source_release(overlaySource_);
		overlaySource_ = nullptr;
	}
}

// Settings that make stream mode worse, checked as each stream starts:
// - the keyframe interval is the step the delay moves in; "auto" (0) is 4-10 s
//   on most encoders;
// - OBS's own Stream Delay adds its fixed delay on top (and stops OBS's
//   output from reaching the relay live).
void StreamDelayController::CheckObsSettings(obs_output_t *output)
{
	warnings_.clear();
	if (obs_encoder_t *encoder = output ? obs_output_get_video_encoder(output) : nullptr) {
		obs_data_t *settings = obs_encoder_get_settings(encoder);
		if (settings && obs_data_has_user_value(settings, "keyint_sec")) {
			const long long keyint = obs_data_get_int(settings, "keyint_sec");
			if (keyint == 0)
				warnings_.push_back("keyint_auto");
			else if (keyint > 4)
				warnings_.push_back("keyint_long:" + std::to_string(keyint));
		} else if (settings) {
			warnings_.push_back("keyint_auto");
		}
		obs_data_release(settings);
	}
	if (config_t *profile = obs_frontend_get_profile_config()) {
		if (config_get_bool(profile, "Output", "DelayEnable"))
			warnings_.push_back("obs_delay");
	}
	for (const std::string &w : warnings_)
		TRIGGLOW_LOG_INFO(kComponent, "settings check: %s", w.c_str());
}

void StreamDelayController::SetApplyTo(ApplyTo applyTo)
{
	if (applyTo_ == applyTo)
		return;
	applyTo_ = applyTo;
	TRIGGLOW_LOG_INFO(kComponent, "delay applies to: %s (stream mode %s from the next stream)",
			  applyTo == ApplyTo::Stream ? "only the stream" : "all of OBS",
			  applyTo == ApplyTo::Stream ? "on" : "off");
	Notify();
}

void StreamDelayController::SetDelaySeconds(uint32_t seconds)
{
	seconds = relay::ClampDelaySeconds(seconds);
	if (seconds == 0)
		seconds = 1;
	if (seconds == delaySeconds_)
		return;
	delaySeconds_ = seconds;
	if (engaged_ && delayOn_)
		relay_.SetDelay(EffectiveDelay());
	Notify();
}

void StreamDelayController::SetKeepSeconds(uint32_t seconds)
{
	keepSeconds_ = relay::ClampDelaySeconds(seconds);
	Notify();
}

void StreamDelayController::SetDrainOnStop(bool drain)
{
	drainOnStop_ = drain;
	Notify();
}

void StreamDelayController::SetBufferStorage(relay::BufferStorage storage)
{
	bufferStorage_ = storage;
	Notify();
}

bool StreamDelayController::Enable()
{
	if (authorized_ && !authorized_()) {
		TRIGGLOW_LOG_INFO(kComponent, "enable refused: sign in with the free account first");
		Notify();
		return false;
	}
	if (delayOn_)
		return true;
	delayOn_ = true;
	if (engaged_)
		relay_.SetDelay(EffectiveDelay());
	TRIGGLOW_LOG_INFO(kComponent, "delay on (%us)%s", delaySeconds_, engaged_ ? "" : ", armed for the next stream");
	Notify();
	return true;
}

void StreamDelayController::Disable()
{
	if (!delayOn_)
		return;
	delayOn_ = false;
	dumpRestoreAtNs_ = 0;
	if (engaged_)
		relay_.SetDelay(0);
	TRIGGLOW_LOG_INFO(kComponent, "delay off");
	Notify();
}

void StreamDelayController::Toggle()
{
	if (delayOn_)
		Disable();
	else
		Enable();
}

void StreamDelayController::AdjustDelay(int deltaSeconds)
{
	const int64_t next = static_cast<int64_t>(delaySeconds_) + deltaSeconds;
	SetDelaySeconds(relay::ClampDelaySeconds(next < 1 ? 1 : next));
}

void StreamDelayController::CutDrain()
{
	if (!draining_)
		return;
	TRIGGLOW_LOG_INFO(kComponent, "sending the last seconds cut by the streamer");
	relay_.Stop();
	draining_ = false;
	Notify();
}

bool StreamDelayController::HandlesControls() const
{
	return ControlsGoToStream(applyTo_, relay::RelayAvailable());
}

StreamDelayStatus StreamDelayController::GetStatus() const
{
	StreamDelayStatus status;
	status.available = relay::RelayAvailable();
	status.applyTo = applyTo_;
	status.engaged = engaged_;
	status.delayOn = delayOn_;
	status.delaySeconds = delaySeconds_;
	status.keepSeconds = RamKeepSeconds();
	status.drainOnStop = drainOnStop_;
	status.bufferStorage = bufferStorage_;
	status.driveKind = driveKind_;
	status.bufferOnDisk = relay::SpillsToDisk(bufferStorage_, driveKind_);
	status.streamBufferOnDisk = streamOnDisk_;
	status.ineligible = ineligible_;
	if (draining_)
		status.state = StreamDelayState::Draining;
	else if (engaged_)
		status.state = delayOn_ ? StreamDelayState::Delayed : StreamDelayState::Live;
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		status.lastEvent = lastEvent_;
	}
	status.warnings = warnings_;
	status.showOverlay = showOverlay_;
	status.overlayCorner = overlayCorner_;
	status.ruleScene = ruleScene_;
	if (dumpRestoreAtNs_ > 0) {
		const int64_t left = dumpRestoreAtNs_ - static_cast<int64_t>(os_gettime_ns());
		status.dumpRestoreIn = left > 0 ? static_cast<uint32_t>((left + 999999999LL) / 1000000000LL) : 0;
	}
	// OBS's own Stream Delay is checked live too: it's the one setting worth
	// fixing before a stream starts.
	if (config_t *profile = obs_frontend_get_profile_config()) {
		if (config_get_bool(profile, "Output", "DelayEnable") &&
		    std::find(status.warnings.begin(), status.warnings.end(), "obs_delay") == status.warnings.end())
			status.warnings.push_back("obs_delay");
	}
	if (engaged_ || draining_)
		status.relay = relay_.GetStatus();
	return status;
}

void StreamDelayController::FrontendEvent(int event, void *param)
{
	auto *self = static_cast<StreamDelayController *>(param);
	switch (static_cast<enum obs_frontend_event>(event)) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		self->OnStreamingStarting();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		self->OnStreamingStopped();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		self->ApplySceneRule();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		self->VerifyOutputUsesRelay();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		self->RestoreFrontendService();
		self->relay_.Stop();
		self->engaged_ = false;
		self->draining_ = false;
		break;
	default:
		break;
	}
}

void StreamDelayController::OnStreamingStarting()
{
	// A new stream cuts whatever the previous one was still sending.
	if (draining_ || relay_.Running()) {
		relay_.Stop();
		draining_ = false;
	}
	engaged_ = false;
	ineligible_.clear();
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		lastEvent_.clear();
	}
	if (applyTo_ != ApplyTo::Stream) {
		Notify();
		return;
	}
	if (!relay::RelayAvailable()) {
		ineligible_ = "unavailable";
		Notify();
		return;
	}

	obs_output_t *output = obs_frontend_get_streaming_output();
	obs_service_t *service = obs_frontend_get_streaming_service();
	const char *outputId = output ? obs_output_get_id(output) : nullptr;
	const char *outputName = output ? obs_output_get_name(output) : nullptr;
	const char *protocol = service ? obs_service_get_protocol(service) : nullptr;
	std::string server;
	std::string key;
	if (service) {
		const char *s = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const char *k = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
		server = s ? s : "";
		key = k ? k : "";
	}

	if (!output || !outputId || std::strcmp(outputId, "rtmp_output") != 0)
		ineligible_ = "output";
	else if (outputName && std::strstr(outputName, "multitrack"))
		ineligible_ = "multitrack";
	else if (!protocol || !(std::strcmp(protocol, "RTMP") == 0 || std::strcmp(protocol, "RTMPS") == 0))
		ineligible_ = "protocol";
	else if (server.empty() || !(StartsWith(server.c_str(), "rtmp://") || StartsWith(server.c_str(), "rtmps://")))
		ineligible_ = "server";

	if (!ineligible_.empty()) {
		TRIGGLOW_LOG_INFO(kComponent,
				  "this stream can't use stream mode (%s: output %s, protocol %s); scene mode "
				  "stays available",
				  ineligible_.c_str(), outputId ? outputId : "none", protocol ? protocol : "none");
		obs_output_release(output);
		Notify();
		return;
	}

	std::vector<relay::Destination> destinations;
	relay::Destination primary;
	primary.name = ServiceDisplayName(service, server);
	primary.server = server;
	primary.key = key;
	primary.primary = true;
	destinations.push_back(primary);

	relay::RelayOptions options;
	options.delaySeconds = EffectiveDelay();
	options.keepSeconds = RamKeepSeconds();
	options.drainOnStop = drainOnStop_;
	// Where the delay buffer lives for this stream. The drive is checked again
	// (the temp folder may have moved); the folder itself is never logged.
	driveKind_ = relay::DetectDriveKind(relay::SpillFile::TempDirectory());
	options.spillToDisk = relay::SpillsToDisk(bufferStorage_, driveKind_);
	streamOnDisk_ = options.spillToDisk;
	TRIGGLOW_LOG_INFO(kComponent, "delay buffer: %s (choice %s, temp folder drive %s)",
			  options.spillToDisk ? "disk" : "memory", relay::BufferStorageName(bufferStorage_),
			  relay::DriveKindName(driveKind_));
	std::string loopServer;
	std::string loopKey;
	std::string error;
	if (!relay_.Start(destinations, options, loopServer, loopKey, error)) {
		TRIGGLOW_LOG_ERROR(kComponent, "couldn't start the relay (%s); this stream goes out without it",
				   error.c_str());
		ineligible_ = "relay";
		{
			std::lock_guard<std::mutex> lock(eventMutex_);
			lastEvent_ = error;
		}
		obs_output_release(output);
		Notify();
		return;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", loopServer.c_str());
	obs_data_set_string(settings, "key", loopKey.c_str());
	obs_data_set_bool(settings, "use_auth", false);
	if (loopbackService_)
		obs_service_update(loopbackService_, settings);
	else
		loopbackService_ = obs_service_create_private("rtmp_custom", "trigglow-dynamic-delay-relay", settings);
	obs_data_release(settings);
	// Simple output mode set the service on the output before this event;
	// Advanced mode sets it right after (AdvancedOutput::StartStreaming
	// calls obs_output_set_service(output, <frontend service>)), which would
	// send the stream straight to the platform. So the output gets the relay
	// now, and the frontend's service is swapped for it until OBS has started
	// the output (same UI call, see RestoreFrontendService).
	obs_output_set_service(output, loopbackService_);
	SwapFrontendService();
	CheckObsSettings(output);
	obs_output_release(output);

	engaged_ = true;
	dumpRestoreAtNs_ = 0;
	TRIGGLOW_LOG_INFO(kComponent, "this stream goes through the relay to %s, delay %us", primary.name.c_str(),
			  EffectiveDelay());
	// The scene the stream starts on may have a rule.
	ApplySceneRule();
	Notify();
}

void StreamDelayController::OnStreamingStopped()
{
	if (!engaged_)
		return;
	engaged_ = false;
	RestoreFrontendService();
	const relay::RelayStatus relayStatus = relay_.GetStatus();
	if (relay_.Running() && !relayStatus.ingestConnected && !relayStatus.ingestEnded) {
		// OBS's stream never reached the relay (it failed to connect, or went
		// elsewhere): nothing to send, and the listener would wait forever.
		TRIGGLOW_LOG_WARN(kComponent, "OBS stopped before its stream reached the relay; nothing to send");
		relay_.Stop();
	} else if (relay_.Running() && !relay_.Finished()) {
		draining_ = true;
		TRIGGLOW_LOG_INFO(kComponent, "OBS stopped; sending the %us viewers haven't seen yet", relay_.Delay());
	} else {
		relay_.Stop();
	}
	Notify();
}

void StreamDelayController::SwapFrontendService()
{
	obs_service_t *current = obs_frontend_get_streaming_service();
	if (!current || current == loopbackService_ || savedFrontendService_)
		return;
	savedFrontendService_ = obs_service_get_ref(current);
	if (!savedFrontendService_)
		return;
	obs_frontend_set_streaming_service(loopbackService_);
	// Back right after OBS's start call returns to the event loop: the output
	// keeps its own reference to the relay's service (reconnects included),
	// and the streamer's settings never show or save the relay.
	if (timer_)
		QTimer::singleShot(0, timer_.get(), [this] { RestoreFrontendService(); });
}

void StreamDelayController::RestoreFrontendService()
{
	if (!savedFrontendService_)
		return;
	if (obs_frontend_get_streaming_service() == loopbackService_)
		obs_frontend_set_streaming_service(savedFrontendService_);
	obs_service_release(savedFrontendService_);
	savedFrontendService_ = nullptr;
	if (engaged_)
		VerifyOutputUsesRelay();
}

// A stream that goes out without the relay must not look delayed: if OBS's
// output ended up with another service, stream mode lets go of this stream.
void StreamDelayController::VerifyOutputUsesRelay()
{
	if (!engaged_ || !loopbackService_)
		return;
	obs_output_t *output = obs_frontend_get_streaming_output();
	if (!output)
		return;
	obs_service_t *service = obs_output_get_service(output);
	obs_output_release(output);
	if (service == loopbackService_)
		return;
	TRIGGLOW_LOG_ERROR(kComponent, "OBS's stream output doesn't go through the relay; this stream isn't delayed");
	engaged_ = false;
	ineligible_ = "relay";
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		lastEvent_ = "OBS's stream doesn't go through the relay";
	}
	relay_.Stop();
	Notify();
}

void StreamDelayController::Tick()
{
	if (draining_ && relay_.Finished()) {
		relay_.Stop();
		draining_ = false;
		TRIGGLOW_LOG_INFO(kComponent, "the last seconds were sent; stream finished");
	}
	if (dumpRestoreAtNs_ > 0 && static_cast<int64_t>(os_gettime_ns()) >= dumpRestoreAtNs_) {
		dumpRestoreAtNs_ = 0;
		if (engaged_ && delayOn_) {
			relay_.SetDelay(delaySeconds_);
			TRIGGLOW_LOG_INFO(kComponent, "dump: the %us delay is back", delaySeconds_);
		}
	}
	Notify();
}

void StreamDelayController::Notify()
{
	UpdateOverlay();
	if (onStatus_)
		onStatus_();
}

} // namespace trigglow
