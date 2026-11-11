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

#include <cstring>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
}

#include "logging.hpp"
#include "packet-delay-core.hpp"

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
	TRIGGLOW_LOG_INFO(kComponent, "stream mode %s (%s)", relay::RelayAvailable() ? "available" : "unavailable",
			  modeEnabled_ ? "on" : "off");
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
	relay_.Stop();
	if (loopbackService_) {
		obs_service_release(loopbackService_);
		loopbackService_ = nullptr;
	}
	engaged_ = false;
}

void StreamDelayController::SetModeEnabled(bool enabled)
{
	if (modeEnabled_ == enabled)
		return;
	modeEnabled_ = enabled;
	TRIGGLOW_LOG_INFO(kComponent, "stream mode %s (applies from the next stream)", enabled ? "on" : "off");
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

void StreamDelayController::SetExtraDestinations(std::vector<ExtraDestination> destinations)
{
	extras_ = std::move(destinations);
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
	if (!modeEnabled_ || !relay::RelayAvailable())
		return false;
	// Streaming without the relay (ineligible service): scene mode handles it.
	if (obs_frontend_streaming_active() && !engaged_)
		return false;
	return true;
}

StreamDelayStatus StreamDelayController::GetStatus() const
{
	StreamDelayStatus status;
	status.available = relay::RelayAvailable();
	status.modeEnabled = modeEnabled_;
	status.engaged = engaged_;
	status.delayOn = delayOn_;
	status.delaySeconds = delaySeconds_;
	status.keepSeconds = keepSeconds_;
	status.drainOnStop = drainOnStop_;
	status.ineligible = ineligible_;
	if (draining_)
		status.state = StreamDelayState::Draining;
	else if (engaged_)
		status.state = delayOn_ ? StreamDelayState::Delayed : StreamDelayState::Live;
	{
		std::lock_guard<std::mutex> lock(eventMutex_);
		status.lastEvent = lastEvent_;
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
	case OBS_FRONTEND_EVENT_EXIT:
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
	if (!modeEnabled_) {
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
		TRIGGLOW_LOG_INFO(kComponent, "this stream can't use stream mode (%s: output %s, protocol %s); scene mode "
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
	for (const ExtraDestination &extra : extras_) {
		if (!extra.enabled || extra.server.empty())
			continue;
		destinations.push_back(relay::Destination{extra.name.empty() ? relay::HostOf(extra.server) : extra.name,
							  extra.server, extra.key, false});
	}

	relay::RelayOptions options;
	options.delaySeconds = EffectiveDelay();
	options.keepSeconds = keepSeconds_;
	options.drainOnStop = drainOnStop_;
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
		loopbackService_ =
			obs_service_create_private("rtmp_custom", "trigglow-dynamic-delay-relay", settings);
	obs_data_release(settings);
	obs_output_set_service(output, loopbackService_);
	obs_output_release(output);

	engaged_ = true;
	TRIGGLOW_LOG_INFO(kComponent, "this stream goes through the relay to %s%s, delay %us", primary.name.c_str(),
			  destinations.size() > 1 ? " and more destinations" : "", EffectiveDelay());
	Notify();
}

void StreamDelayController::OnStreamingStopped()
{
	if (!engaged_)
		return;
	engaged_ = false;
	if (relay_.Running() && !relay_.Finished()) {
		draining_ = true;
		TRIGGLOW_LOG_INFO(kComponent, "OBS stopped; sending the %us viewers haven't seen yet",
				  relay_.Delay());
	} else {
		relay_.Stop();
	}
	Notify();
}

void StreamDelayController::Tick()
{
	if (draining_ && relay_.Finished()) {
		relay_.Stop();
		draining_ = false;
		TRIGGLOW_LOG_INFO(kComponent, "the last seconds were sent; stream finished");
	}
	Notify();
}

void StreamDelayController::Notify()
{
	if (onStatus_)
		onStatus_();
}

} // namespace trigglow
