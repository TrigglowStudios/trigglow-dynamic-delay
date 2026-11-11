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

#include <cstddef>

#include "delay-router.hpp"

extern "C" {
#include <obs.h> // obs_hotkey_id, obs_hotkey_t, obs_hotkey_func — core libobs, not the frontend API.
}

// Registers the plugin's native OBS hotkeys via obs_hotkey_register_frontend
// (libobs/obs-hotkey.h). Registered hotkeys show up in OBS's own
// Settings -> Hotkeys list under this plugin's display name, exactly like
// any other native OBS hotkey (Start Streaming, Mute, etc). Stream Deck's own
// "Hotkey" action can trigger them by key combination; the Trigglow Stream
// Deck plugin uses obs-websocket instead (websocket-vendor.hpp).
//
// v1.0.0: every hotkey goes through DelayRouter, which picks stream mode or
// scene mode. New: "Back to live" and "Delay +5 s" / "Delay -5 s".
//
// Frontend hotkeys registered this way are automatically persisted by OBS's
// own hotkey config (keyed by the string name passed to
// obs_hotkey_register_frontend) — this plugin does not need to save/load
// key bindings itself, only unregister them cleanly on unload.
namespace trigglow {

class DelayHotkeys {
public:
	explicit DelayHotkeys(DelayRouter &router);
	~DelayHotkeys();

	DelayHotkeys(const DelayHotkeys &) = delete;
	DelayHotkeys &operator=(const DelayHotkeys &) = delete;

	// Registers every hotkey. Call once from obs_module_load().
	void Init();

	// Unregisters them. Call from obs_module_unload().
	void Shutdown();

	enum class Action { Toggle, Enable, Disable, SkipLive, DelayUp, DelayDown, Count };

private:
	struct Binding {
		DelayHotkeys *self = nullptr;
		Action action = Action::Toggle;
		obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	};

	static void Callback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void RunOnUiThread(void *param);

	DelayRouter &router_;
	Binding bindings_[static_cast<size_t>(Action::Count)];
};

} // namespace trigglow
