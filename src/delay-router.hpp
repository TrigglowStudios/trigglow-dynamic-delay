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

// v1.0.0: one entry point for every control -- the dock's one button, OBS
// hotkeys (hotkeys.cpp) and obs-websocket requests from the Stream Deck
// (websocket-vendor.cpp). Each goes to the mode the streamer selected in the
// dock and only to it: stream mode when "Apply the delay only to the stream"
// is ticked and stream mode can run (StreamDelayController::HandlesControls(),
// apply-to.hpp), scene mode otherwise. UI thread only.
//
// Pure (no OBS): the two modes are DelayTargets (delay-targets.hpp wraps the
// controllers), so tests/delay_router_tests.cpp drives it with fakes.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace trigglow {

struct RouterStatus {
	std::string mode;  // "stream" or "scene".
	std::string state; // stream: idle|live|delayed|draining; scene: inactive|filling|active|error.
	bool delayOn = false;
	uint32_t delaySeconds = 0;   // Configured seconds.
	uint32_t appliedSeconds = 0; // What viewers get right now (0 when live).
	double behindSeconds = 0.0;  // Stream mode: age of what the main destination sends.
	double ramMegabytes = 0.0;   // Stream mode: the packet history.
	// v1.0.0, all of OBS: the delayed scenes (in the dock's order) and, while
	// the delay runs, the one viewers see. Filled in either mode.
	std::vector<std::string> delayedScenes;
	std::string delayedScene;
};

// One delay mode as the router sees it.
class DelayTarget {
public:
	virtual ~DelayTarget() = default;
	virtual void Enable() = 0;
	virtual void Disable() = 0;
	virtual void Toggle() = 0;
	virtual void AdjustDelay(int deltaSeconds) = 0;
	virtual void SetDelay(uint32_t seconds) = 0;
	// False when there is nothing to drop (scene mode never has).
	virtual bool Dump() = 0;
	virtual void ApplyPreset(int index) = 0;
	virtual RouterStatus Status() const = 0;
	// v1.0.0: all of OBS only: put one of the delayed scenes in the delay
	// (by name, or by 0-based position). False when there's nothing to do.
	virtual bool SelectDelayedScene(const std::string & /*sceneName*/) { return false; }
	virtual bool SelectDelayedSceneSlot(size_t /*slot*/) { return false; }
};

class DelayRouter {
public:
	// `streamOnly`: true while the controls go to stream mode (the dock's
	// check box, if stream mode can run).
	DelayRouter(DelayTarget &allOfObs, DelayTarget &streamOnly, std::function<bool()> streamSelected)
		: all_(allOfObs),
		  stream_(streamOnly),
		  streamSelected_(std::move(streamSelected))
	{
	}

	void Enable() { Active().Enable(); }
	void Disable() { Active().Disable(); }
	void Toggle() { Active().Toggle(); }
	// Back to live: the delay off, in either mode.
	void SkipToLive() { Disable(); }
	// Both modes change the delay live, 1-600 s.
	void AdjustDelay(int deltaSeconds) { Active().AdjustDelay(deltaSeconds); }
	void SetDelay(uint32_t seconds) { Active().SetDelay(seconds); }
	// Stream mode only: drop what viewers haven't seen yet. False when there
	// is nothing to drop (or in scene mode).
	bool Dump() { return Active().Dump(); }
	// Preset 0-2: its seconds, delay on.
	void ApplyPreset(int index) { Active().ApplyPreset(index); }
	RouterStatus Status() const
	{
		RouterStatus out = streamSelected_() ? stream_.Status() : all_.Status();
		if (streamSelected_()) {
			RouterStatus scene = all_.Status();
			out.delayedScenes = std::move(scene.delayedScenes);
			out.delayedScene = std::move(scene.delayedScene);
		}
		return out;
	}
	// v1.0.0: the delayed scenes belong to all of OBS whatever is selected
	// (only it has them; it does nothing while its delay is off).
	bool SelectDelayedScene(const std::string &sceneName) { return all_.SelectDelayedScene(sceneName); }
	bool SelectDelayedSceneSlot(size_t slot) { return all_.SelectDelayedSceneSlot(slot); }
	bool StreamMode() const { return streamSelected_(); }

private:
	DelayTarget &Active() { return streamSelected_() ? stream_ : all_; }

	DelayTarget &all_;
	DelayTarget &stream_;
	std::function<bool()> streamSelected_;
};

} // namespace trigglow
