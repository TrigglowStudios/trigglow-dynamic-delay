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

// v1.0.0: the delay over obs-websocket, so the Trigglow Stream Deck plugin
// (and anything else that speaks obs-websocket 5) can drive it and show its
// state on a key, with no key combination and no OBS focus needed.
//
// Vendor "trigglow-dynamic-delay", requests (CallVendorRequest):
//   GetStatus                      -> status
//   EnableDelay / DisableDelay / ToggleDelay / SkipToLive -> status
//   SetDelay {"seconds": n}        -> status
//   AdjustDelay {"delta": n}       -> status
// status = {mode, state, delayOn, delaySeconds, appliedSeconds,
//           behindSeconds, ramMegabytes}
// Event (VendorEvent) "StatusChanged" with the same fields when it changes.
//
// obs-websocket ships with OBS; when it's missing or disabled nothing here
// does anything. Requests arrive on obs-websocket's thread and run on OBS's UI
// thread (obs_queue_task, waiting), like the hotkeys.

namespace trigglow {

class DelayRouter;

namespace websocket {

// Call from obs_module_post_load(): obs-websocket registers its API while
// loading, after this plugin's obs_module_load().
void Register(DelayRouter &router);
// Emits StatusChanged when the status differs from the last one sent.
// UI thread; cheap when nothing changed.
void NotifyStatus();
void Unregister();

} // namespace websocket
} // namespace trigglow
