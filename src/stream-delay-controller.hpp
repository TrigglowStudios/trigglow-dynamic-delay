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

// v1.0.0 stream mode, the OBS side. When OBS is about to start streaming
// (OBS_FRONTEND_EVENT_STREAMING_STARTING, before the output starts) and the
// stream is eligible -- OBS's own RTMP/RTMPS output, not Enhanced
// Broadcasting -- it starts the relay (stream-relay.hpp) with the streamer's
// own service as the first destination and points THAT stream's output at the
// relay on 127.0.0.1. The streamer's saved service settings are never touched:
// OBS sets its service on the output again on every start.
//
// From then on the delay is a number the relay applies to the encoded stream:
// on, off, or changed while live (0-600 s), with no reconnect, no scene
// juggling and no second encode. Recording, the Replay Buffer and the virtual
// camera keep the live picture. Every scene is delayed, so switching scenes
// while delayed is safe.
//
// Not eligible (SRT/WHIP services, Enhanced Broadcasting, a system without the
// relay): the stream goes out exactly as OBS sends it and the dock offers scene
// mode (BufferModeController) as before. All public methods run on OBS's UI
// thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "stream-relay.hpp"

struct obs_service;
typedef struct obs_service obs_service_t;
class QTimer;

namespace trigglow {

enum class StreamDelayState {
	Idle,     // Not streaming through the relay. The delay may be armed for the next stream.
	Live,     // Streaming through the relay, delay off.
	Delayed,  // Streaming through the relay, delay on.
	Draining, // OBS stopped; the relay still sends the last seconds viewers haven't seen.
};

struct StreamDelayStatus {
	bool available = false;   // The relay exists on this system.
	bool modeEnabled = false; // The streamer chose stream mode.
	bool engaged = false;     // The current stream goes through the relay.
	bool delayOn = false;     // Delay requested (armed before a stream, or applied).
	uint32_t delaySeconds = 30;
	uint32_t keepSeconds = 120;
	bool drainOnStop = true;
	StreamDelayState state = StreamDelayState::Idle;
	// Why the current stream couldn't use stream mode (empty when it could).
	std::string ineligible;
	// Last problem worth showing (a destination failed...), empty when none.
	std::string lastEvent;
	relay::RelayStatus relay;
};

struct ExtraDestination {
	std::string name;
	std::string server;
	std::string key;
	bool enabled = true;
};

class StreamDelayController {
public:
	StreamDelayController();
	~StreamDelayController();

	void Init();
	void Shutdown();

	// Settings (persisted by plugin-main.cpp).
	void SetModeEnabled(bool enabled);
	bool ModeEnabled() const { return modeEnabled_; }
	void SetDelaySeconds(uint32_t seconds); // Applied live while the delay is on.
	uint32_t DelaySeconds() const { return delaySeconds_; }
	void SetKeepSeconds(uint32_t seconds);
	uint32_t KeepSeconds() const { return keepSeconds_; }
	void SetDrainOnStop(bool drain);
	bool DrainOnStop() const { return drainOnStop_; }
	void SetExtraDestinations(std::vector<ExtraDestination> destinations);
	const std::vector<ExtraDestination> &ExtraDestinations() const { return extras_; }

	// Controls. Enable is gated by the free account like scene mode.
	bool Enable();
	void Disable();
	void Toggle();
	void AdjustDelay(int deltaSeconds);
	// Stops sending the last seconds after OBS stopped (Draining) at once.
	void CutDrain();
	bool DelayOn() const { return delayOn_; }

	// True when the hotkeys and Stream Deck should drive this controller and
	// not scene mode: stream mode is on and the stream (current or next) can
	// use it.
	bool HandlesControls() const;
	bool Engaged() const { return engaged_; }

	StreamDelayStatus GetStatus() const;
	void SetStatusCallback(std::function<void()> callback) { onStatus_ = std::move(callback); }
	void SetAuthorizationCheck(std::function<bool()> check) { authorized_ = std::move(check); }

	// obs_frontend_event trampoline target (the enum is passed as int to keep
	// obs-frontend-api.h out of this header).
	static void FrontendEvent(int event, void *param);

private:
	void OnStreamingStarting();
	void OnStreamingStopped();
	void Tick();
	void Notify();
	uint32_t EffectiveDelay() const { return delayOn_ ? delaySeconds_ : 0; }

	relay::StreamRelay relay_;
	obs_service_t *loopbackService_ = nullptr;
	std::unique_ptr<QTimer> timer_;
	bool initialized_ = false;
	bool modeEnabled_ = true;
	bool delayOn_ = false;
	uint32_t delaySeconds_ = 30;
	uint32_t keepSeconds_ = 120;
	bool drainOnStop_ = true;
	bool engaged_ = false;
	bool draining_ = false;
	std::string ineligible_;
	std::vector<ExtraDestination> extras_;
	std::function<void()> onStatus_;
	std::function<bool()> authorized_;

	mutable std::mutex eventMutex_;
	std::string lastEvent_;
};

} // namespace trigglow
