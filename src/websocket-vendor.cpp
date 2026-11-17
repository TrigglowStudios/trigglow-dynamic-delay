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

#include "websocket-vendor.hpp"

#include <cmath>
#include <cstring>
#include <string>

extern "C" {
#include <callback/calldata.h>
#include <callback/proc.h>
#include <obs.h>
}

#include "delay-router.hpp"
#include "logging.hpp"

namespace trigglow::websocket {

namespace {

constexpr const char *kComponent = "websocket";
constexpr const char *kVendorName = "trigglow-dynamic-delay";

// The obs-websocket 5 plugin API, through its proc handler (what
// obs-websocket-api.h does; reimplemented here to keep the header out of the
// build).
typedef void (*RequestFn)(obs_data_t *request, obs_data_t *response, void *priv);
struct RequestCallback {
	RequestFn callback;
	void *priv;
};

proc_handler_t *g_ph = nullptr;
void *g_vendor = nullptr;
DelayRouter *g_router = nullptr;
std::string g_lastSent;

proc_handler_t *WebsocketProcHandler()
{
	proc_handler_t *global = obs_get_proc_handler();
	if (!global)
		return nullptr;
	calldata_t cd = {};
	proc_handler_t *ph = nullptr;
	if (proc_handler_call(global, "obs_websocket_api_get_ph", &cd))
		ph = static_cast<proc_handler_t *>(calldata_ptr(&cd, "ph"));
	calldata_free(&cd);
	return ph;
}

void FillStatus(obs_data_t *out)
{
	if (!g_router)
		return;
	const RouterStatus s = g_router->Status();
	obs_data_set_string(out, "mode", s.mode.c_str());
	obs_data_set_string(out, "state", s.state.c_str());
	obs_data_set_bool(out, "delayOn", s.delayOn);
	obs_data_set_int(out, "delaySeconds", s.delaySeconds);
	obs_data_set_int(out, "appliedSeconds", s.appliedSeconds);
	obs_data_set_double(out, "behindSeconds", std::round(s.behindSeconds * 10.0) / 10.0);
	obs_data_set_double(out, "ramMegabytes", std::round(s.ramMegabytes * 10.0) / 10.0);
	// v1.0.0: the delayed scenes (all of OBS) and the one in the delay now
	// (empty while it's off).
	obs_data_array_t *scenes = obs_data_array_create();
	for (const std::string &name : s.delayedScenes) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "sceneName", name.c_str());
		obs_data_array_push_back(scenes, item);
		obs_data_release(item);
	}
	obs_data_set_array(out, "delayedScenes", scenes);
	obs_data_array_release(scenes);
	obs_data_set_string(out, "delayedScene", s.delayedScene.c_str());
}

enum class Op {
	Status,
	Enable,
	Disable,
	Toggle,
	SkipToLive,
	SetDelay,
	AdjustDelay,
	Dump,
	ApplyPreset,
	SetDelayedScene
};

struct Job {
	Op op;
	obs_data_t *request;
	obs_data_t *response;
};

void RunJob(void *param)
{
	auto *job = static_cast<Job *>(param);
	if (!g_router)
		return;
	switch (job->op) {
	case Op::Status:
		break;
	case Op::Enable:
		g_router->Enable();
		break;
	case Op::Disable:
		g_router->Disable();
		break;
	case Op::Toggle:
		g_router->Toggle();
		break;
	case Op::SkipToLive:
		g_router->SkipToLive();
		break;
	case Op::SetDelay: {
		const long long seconds = obs_data_get_int(job->request, "seconds");
		if (seconds > 0)
			g_router->SetDelay(static_cast<uint32_t>(seconds));
		break;
	}
	case Op::AdjustDelay:
		g_router->AdjustDelay(static_cast<int>(obs_data_get_int(job->request, "delta")));
		break;
	case Op::Dump:
		obs_data_set_bool(job->response, "dumped", g_router->Dump());
		break;
	case Op::ApplyPreset:
		g_router->ApplyPreset(static_cast<int>(obs_data_get_int(job->request, "index")));
		break;
	case Op::SetDelayedScene: {
		// {"sceneName": "..."} or {"slot": 1..8} (the dock's list, 1-based).
		const char *name = obs_data_get_string(job->request, "sceneName");
		const long long slot = obs_data_get_int(job->request, "slot");
		const bool selected =
			name && *name ? g_router->SelectDelayedScene(name)
				      : slot > 0 && g_router->SelectDelayedSceneSlot(static_cast<size_t>(slot - 1));
		obs_data_set_bool(job->response, "selected", selected);
		break;
	}
	}
	FillStatus(job->response);
	obs_data_set_bool(job->response, "ok", true);
}

template<Op op> void Handle(obs_data_t *request, obs_data_t *response, void *)
{
	Job job{op, request, response};
	obs_queue_task(OBS_TASK_UI, RunJob, &job, true);
}

void RegisterRequest(const char *type, RequestFn fn)
{
	RequestCallback cb{fn, nullptr};
	calldata_t cd = {};
	calldata_set_ptr(&cd, "vendor", g_vendor);
	calldata_set_string(&cd, "type", type);
	calldata_set_ptr(&cd, "callback", &cb);
	proc_handler_call(g_ph, "vendor_request_register", &cd);
	if (!calldata_bool(&cd, "success"))
		TRIGGLOW_LOG_WARN(kComponent, "couldn't register the %s request", type);
	calldata_free(&cd);
}

} // namespace

void Register(DelayRouter &router)
{
	g_router = &router;
	g_ph = WebsocketProcHandler();
	if (!g_ph) {
		TRIGGLOW_LOG_INFO(kComponent, "obs-websocket not available: Stream Deck control over it is off");
		return;
	}
	calldata_t cd = {};
	calldata_set_string(&cd, "name", kVendorName);
	proc_handler_call(g_ph, "vendor_register", &cd);
	g_vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	if (!g_vendor) {
		TRIGGLOW_LOG_WARN(kComponent, "obs-websocket refused the vendor %s", kVendorName);
		return;
	}
	RegisterRequest("GetStatus", &Handle<Op::Status>);
	RegisterRequest("EnableDelay", &Handle<Op::Enable>);
	RegisterRequest("DisableDelay", &Handle<Op::Disable>);
	RegisterRequest("ToggleDelay", &Handle<Op::Toggle>);
	RegisterRequest("SkipToLive", &Handle<Op::SkipToLive>);
	RegisterRequest("SetDelay", &Handle<Op::SetDelay>);
	RegisterRequest("AdjustDelay", &Handle<Op::AdjustDelay>);
	RegisterRequest("DumpDelay", &Handle<Op::Dump>);
	RegisterRequest("ApplyPreset", &Handle<Op::ApplyPreset>);
	RegisterRequest("SetDelayedScene", &Handle<Op::SetDelayedScene>);
	TRIGGLOW_LOG_INFO(kComponent, "obs-websocket vendor \"%s\" registered", kVendorName);
}

void NotifyStatus()
{
	if (!g_ph || !g_vendor || !g_router)
		return;
	obs_data_t *data = obs_data_create();
	FillStatus(data);
	// behindSeconds moves all the time; the event is for state changes.
	obs_data_erase(data, "behindSeconds");
	obs_data_erase(data, "ramMegabytes");
	const char *json = obs_data_get_json(data);
	const std::string snapshot = json ? json : "";
	if (snapshot != g_lastSent) {
		g_lastSent = snapshot;
		calldata_t cd = {};
		calldata_set_ptr(&cd, "vendor", g_vendor);
		calldata_set_string(&cd, "type", "StatusChanged");
		calldata_set_ptr(&cd, "data", data);
		proc_handler_call(g_ph, "vendor_event_emit", &cd);
		calldata_free(&cd);
	}
	obs_data_release(data);
}

void Unregister()
{
	// obs-websocket drops vendors with their module; just stop answering.
	g_router = nullptr;
	g_vendor = nullptr;
	g_ph = nullptr;
}

} // namespace trigglow::websocket
