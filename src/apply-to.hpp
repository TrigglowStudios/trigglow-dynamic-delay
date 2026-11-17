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

// v1.0.0: what the delay applies to, the dock's one advanced choice. Pure, no
// OBS (tests/apply_to_tests.cpp).
//
// - All (the default): all of OBS, scene mode (BufferModeController). The
//   loading scene shows while the buffer fills, then Program, the stream AND
//   the recording are delayed, as in 0.7.
// - Stream: only the stream, stream mode (StreamDelayController). OBS's RTMP
//   output goes through the loopback relay; recording and preview stay live.
//
// Saved as "apply_to": "all" | "stream" in stream-mode-settings.json. Test
// builds of 1.0 saved "mode_enabled" (true by default) instead; 1.0 was never
// released, so that key is ignored and everyone starts on All.

#include <string>

namespace trigglow {

enum class ApplyTo { All, Stream };

// The JSON type of "apply_to" as read from the settings file.
enum class ApplyToType { Missing, String, Other };

struct ApplyToSetting {
	ApplyTo value = ApplyTo::All;
	// A "mode_enabled" from a 1.0 test build was found and dropped (it is not
	// written back): the caller logs it once.
	bool droppedModeEnabled = false;
	// "apply_to" was there but is neither "all" nor "stream": All, with a WARN.
	bool warn = false;
};

// `type`/`text`: the "apply_to" item. `hasModeEnabled`: the file still has the
// test builds' "mode_enabled" key (whatever its value).
ApplyToSetting ParseApplyTo(ApplyToType type, const std::string &text, bool hasModeEnabled);

// "all" or "stream", what SaveStreamSettings() writes.
const char *ApplyToName(ApplyTo value);

// Whether the hotkeys, obs-websocket, the Stream Deck and the dock's button
// drive stream mode: only when the streamer chose "only the stream" AND stream
// mode can run on this system (the relay exists). Otherwise scene mode, which
// is also what the dock shows then.
bool ControlsGoToStream(ApplyTo choice, bool streamModeAvailable);

} // namespace trigglow
