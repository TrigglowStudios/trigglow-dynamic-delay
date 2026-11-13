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

#include "delay-targets.hpp"

#include <algorithm>

#include "buffer-mode-controller.hpp"
#include "packet-delay-core.hpp"
#include "stream-delay-controller.hpp"

namespace trigglow {

namespace {

uint32_t ClampScene(int64_t seconds)
{
	return static_cast<uint32_t>(std::clamp<int64_t>(seconds, BufferModeController::kMinDelaySeconds,
							 BufferModeController::kMaxDelaySeconds));
}

} // namespace

// ---- All of OBS (scene mode) ----------------------------------------------

void SceneDelayTarget::Enable()
{
	scene_.Enable();
}

void SceneDelayTarget::Disable()
{
	scene_.Disable();
}

void SceneDelayTarget::Toggle()
{
	scene_.Toggle();
}

void SceneDelayTarget::AdjustDelay(int deltaSeconds)
{
	scene_.SetDelaySeconds(ClampScene(static_cast<int64_t>(scene_.GetStatus().delaySeconds) + deltaSeconds));
}

void SceneDelayTarget::SetDelay(uint32_t seconds)
{
	scene_.SetDelaySeconds(ClampScene(seconds));
}

void SceneDelayTarget::ApplyPreset(int index)
{
	if (index < 0 || index > 2)
		return;
	scene_.SetDelaySeconds(ClampScene(presets_.Preset(index)));
	scene_.Enable();
}

RouterStatus SceneDelayTarget::Status() const
{
	RouterStatus out;
	const BufferModeStatus s = scene_.GetStatus();
	out.mode = "scene";
	switch (s.state) {
	case BufferModeState::Inactive:
		out.state = "inactive";
		break;
	case BufferModeState::Filling:
		out.state = "filling";
		break;
	case BufferModeState::Active:
		out.state = "active";
		break;
	case BufferModeState::Error:
		out.state = "error";
		break;
	}
	out.delayOn = s.state == BufferModeState::Filling || s.state == BufferModeState::Active;
	out.delaySeconds = s.delaySeconds;
	out.appliedSeconds = s.state == BufferModeState::Active ? s.effectiveDelaySeconds : 0;
	return out;
}

// ---- Only the stream (stream mode) ----------------------------------------

void StreamDelayTarget::Enable()
{
	stream_.Enable();
}

void StreamDelayTarget::Disable()
{
	stream_.Disable();
}

void StreamDelayTarget::Toggle()
{
	stream_.Toggle();
}

void StreamDelayTarget::AdjustDelay(int deltaSeconds)
{
	stream_.AdjustDelay(deltaSeconds);
}

void StreamDelayTarget::SetDelay(uint32_t seconds)
{
	stream_.SetDelaySeconds(seconds);
}

bool StreamDelayTarget::Dump()
{
	return stream_.Dump();
}

void StreamDelayTarget::ApplyPreset(int index)
{
	stream_.ApplyPreset(index);
}

RouterStatus StreamDelayTarget::Status() const
{
	RouterStatus out;
	const StreamDelayStatus s = stream_.GetStatus();
	out.mode = "stream";
	switch (s.state) {
	case StreamDelayState::Idle:
		out.state = "idle";
		break;
	case StreamDelayState::Live:
		out.state = "live";
		break;
	case StreamDelayState::Delayed:
		out.state = "delayed";
		break;
	case StreamDelayState::Draining:
		out.state = "draining";
		break;
	}
	out.delayOn = s.delayOn;
	out.delaySeconds = s.delaySeconds;
	out.appliedSeconds = s.engaged ? s.relay.delaySeconds : 0;
	for (const relay::SinkStatus &sink : s.relay.sinks) {
		if (sink.primary)
			out.behindSeconds = sink.behindSeconds;
	}
	out.ramMegabytes = static_cast<double>(s.relay.historyBytes) / (1024.0 * 1024.0);
	return out;
}

} // namespace trigglow
