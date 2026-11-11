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

// v1.0.0: one entry point for every control that isn't the dock's own
// buttons -- OBS hotkeys (hotkeys.cpp) and obs-websocket requests from the
// Stream Deck (websocket-vendor.cpp). It sends each one to stream mode when
// that handles the controls (StreamDelayController::HandlesControls()), to
// scene mode otherwise. UI thread only.

#include <cstdint>
#include <string>

namespace trigglow {

class BufferModeController;
class StreamDelayController;

struct RouterStatus {
	std::string mode;  // "stream" or "scene".
	std::string state; // stream: idle|live|delayed|draining; scene: inactive|filling|active|error.
	bool delayOn = false;
	uint32_t delaySeconds = 0;   // Configured seconds.
	uint32_t appliedSeconds = 0; // What viewers get right now (0 when live).
	double behindSeconds = 0.0;  // Stream mode: age of what the main destination sends.
	double ramMegabytes = 0.0;   // Stream mode: the packet history.
};

class DelayRouter {
public:
	DelayRouter(BufferModeController &scene, StreamDelayController &stream) : scene_(scene), stream_(stream) {}

	void Enable();
	void Disable();
	void Toggle();
	// Stream mode: back to live (delay off). Scene mode: Disable.
	void SkipToLive();
	// Stream mode only (live); scene mode changes the seconds for the next Enable.
	void AdjustDelay(int deltaSeconds);
	void SetDelay(uint32_t seconds);
	RouterStatus Status() const;
	bool StreamMode() const;

private:
	BufferModeController &scene_;
	StreamDelayController &stream_;
};

} // namespace trigglow
