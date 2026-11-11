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
struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_scene;
typedef struct obs_scene obs_scene_t;
struct obs_source;
typedef struct obs_source obs_source_t;
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
	// OBS settings worth changing, as locale key suffixes under
	// "Stream.Warn." with an optional ":<value>" (e.g. "keyint_long:8").
	std::vector<std::string> warnings;
	bool showOverlay = true;
	uint32_t overlayCorner = 1;
	// Seconds until the delay comes back after a Dump (0: no dump pending).
	uint32_t dumpRestoreIn = 0;
	// The scene rule applied last (empty: none).
	std::string ruleScene;
	relay::RelayStatus relay;
};

// What switching OBS's program to a scene does to the delay (stream mode).
enum class SceneAction : uint32_t { None = 0, DelayOn = 1, DelayOff = 2 };

struct SceneRule {
	std::string scene;
	SceneAction action = SceneAction::None;
	uint32_t seconds = 0; // DelayOn: these seconds (0 = keep the current ones).
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
	// The "DELAY 30 s" badge, drawn over OBS's program on a free output
	// channel while the delay is on (viewers get it with the delayed picture).
	void SetShowOverlay(bool show);
	bool ShowOverlay() const { return showOverlay_; }
	void SetOverlayCorner(uint32_t corner);
	uint32_t OverlayCorner() const { return overlayCorner_; }
	// Path of the badge PNG for `seconds` (overlay-badge.hpp), UI thread.
	void SetOverlayImageProvider(std::function<std::string(uint32_t)> provider)
	{
		overlayImage_ = std::move(provider);
	}

	// Controls. Enable is gated by the free account like scene mode.
	bool Enable();
	void Disable();
	void Toggle();
	void AdjustDelay(int deltaSeconds);
	// Stops sending the last seconds after OBS stopped (Draining) at once.
	void CutDrain();
	// Dump: what viewers haven't seen yet is never sent (something slipped on
	// screen). The stream jumps to live and the delay comes back on its own
	// after its seconds, rewinding only into what came after the dump. False
	// when there is nothing to dump (not streaming through the relay, or the
	// delay is off).
	bool Dump();

	// Presets 1-3 (seconds), each with a hotkey: set the seconds and turn the
	// delay on.
	void SetPreset(int index, uint32_t seconds);
	uint32_t Preset(int index) const;
	void ApplyPreset(int index);

	// Scene rules: entering a scene can turn the delay on (with its seconds)
	// or off. Applied when OBS's program scene changes and when a stream
	// starts, in stream mode only (scene mode delays one chosen scene).
	void SetSceneRules(std::vector<SceneRule> rules);
	const std::vector<SceneRule> &SceneRules() const { return sceneRules_; }
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
	void UpdateOverlay();
	void ClearOverlay();
	void CheckObsSettings(obs_output_t *output);
	void SwapFrontendService();
	void RestoreFrontendService();
	void VerifyOutputUsesRelay();
	uint32_t EffectiveDelay() const { return delayOn_ ? delaySeconds_ : 0; }

	void ApplySceneRule();

	uint32_t presets_[3] = {10, 30, 60};
	std::vector<SceneRule> sceneRules_;
	std::string ruleScene_;
	int64_t dumpRestoreAtNs_ = 0; // Monotonic; 0 = no dump pending.

	bool showOverlay_ = true;
	uint32_t overlayCorner_ = 1;
	std::function<std::string(uint32_t)> overlayImage_;
	obs_scene_t *overlayScene_ = nullptr;
	obs_source_t *overlaySource_ = nullptr;
	uint32_t overlaySeconds_ = 0;
	bool overlayShown_ = false;
	std::vector<std::string> warnings_;

	relay::StreamRelay relay_;
	obs_service_t *loopbackService_ = nullptr;
	// The streamer's own service while the frontend's is swapped for the
	// relay's (Advanced output mode, see OnStreamingStarting). Holds a ref.
	obs_service_t *savedFrontendService_ = nullptr;
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
