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

#include "hotkeys.hpp"
#include "i18n.hpp"
#include "logging.hpp"

namespace trigglow {

namespace {
constexpr const char *kComponent = "hotkeys";

struct HotkeyDef {
	DelayHotkeys::Action action;
	const char *name;
	const char *textKey;
};

// The first three names are unchanged since v0.2.0, so existing key
// bindings (and Stream Deck "Hotkey" buttons) keep working.
constexpr HotkeyDef kHotkeys[] = {
	{DelayHotkeys::Action::Toggle, "trigglow_dynamic_delay.toggle", "Hotkey.Toggle"},
	{DelayHotkeys::Action::Enable, "trigglow_dynamic_delay.enable", "Hotkey.Enable"},
	{DelayHotkeys::Action::Disable, "trigglow_dynamic_delay.disable", "Hotkey.Disable"},
	{DelayHotkeys::Action::SkipLive, "trigglow_dynamic_delay.skip_live", "Hotkey.SkipLive"},
	{DelayHotkeys::Action::DelayUp, "trigglow_dynamic_delay.delay_up", "Hotkey.DelayUp"},
	{DelayHotkeys::Action::DelayDown, "trigglow_dynamic_delay.delay_down", "Hotkey.DelayDown"},
	{DelayHotkeys::Action::Dump, "trigglow_dynamic_delay.dump", "Hotkey.Dump"},
	{DelayHotkeys::Action::Preset1, "trigglow_dynamic_delay.preset1", "Hotkey.Preset1"},
	{DelayHotkeys::Action::Preset2, "trigglow_dynamic_delay.preset2", "Hotkey.Preset2"},
	{DelayHotkeys::Action::Preset3, "trigglow_dynamic_delay.preset3", "Hotkey.Preset3"},
};
} // namespace

DelayHotkeys::DelayHotkeys(DelayRouter &router) : router_(router) {}

DelayHotkeys::~DelayHotkeys()
{
	Shutdown();
}

void DelayHotkeys::Init()
{
	for (const HotkeyDef &def : kHotkeys) {
		Binding &b = bindings_[static_cast<size_t>(def.action)];
		b.self = this;
		b.action = def.action;
		b.id = obs_hotkey_register_frontend(def.name, Str(def.textKey), &DelayHotkeys::Callback, &b);
	}
	TRIGGLOW_LOG_INFO(kComponent, "registered %zu hotkeys — assign them in Settings > Hotkeys",
			  sizeof(kHotkeys) / sizeof(kHotkeys[0]));
}

void DelayHotkeys::Shutdown()
{
	for (Binding &b : bindings_) {
		if (b.id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(b.id);
			b.id = OBS_INVALID_HOTKEY_ID;
		}
	}
}

// obs_hotkey_register_frontend callbacks fire on OBS's internal hotkey
// thread, never the Qt UI thread. Both controllers (and, through their status
// callbacks, the dock's widgets) live on the UI thread, so the call goes
// through OBS's own UI task queue.
void DelayHotkeys::Callback(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hotkey*/, bool pressed)
{
	// Key-down only, exactly like OBS's own built-in hotkeys.
	if (!pressed)
		return;
	obs_queue_task(OBS_TASK_UI, &DelayHotkeys::RunOnUiThread, data, false);
}

void DelayHotkeys::RunOnUiThread(void *param)
{
	auto *b = static_cast<Binding *>(param);
	DelayRouter &router = b->self->router_;
	switch (b->action) {
	case Action::Toggle:
		router.Toggle();
		break;
	case Action::Enable:
		router.Enable();
		break;
	case Action::Disable:
		router.Disable();
		break;
	case Action::SkipLive:
		router.SkipToLive();
		break;
	case Action::DelayUp:
		router.AdjustDelay(5);
		break;
	case Action::DelayDown:
		router.AdjustDelay(-5);
		break;
	case Action::Dump:
		router.Dump();
		break;
	case Action::Preset1:
		router.ApplyPreset(0);
		break;
	case Action::Preset2:
		router.ApplyPreset(1);
		break;
	case Action::Preset3:
		router.ApplyPreset(2);
		break;
	case Action::Count:
		break;
	}
}

} // namespace trigglow
