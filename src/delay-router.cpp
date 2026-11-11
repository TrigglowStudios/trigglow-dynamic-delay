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

#include "delay-router.hpp"

#include <algorithm>

#include "buffer-mode-controller.hpp"
#include "packet-delay-core.hpp"
#include "stream-delay-controller.hpp"

namespace trigglow {

bool DelayRouter::StreamMode() const
{
	return stream_.HandlesControls();
}

void DelayRouter::Enable()
{
	if (StreamMode())
		stream_.Enable();
	else
		scene_.Enable();
}

void DelayRouter::Disable()
{
	if (StreamMode())
		stream_.Disable();
	else
		scene_.Disable();
}

void DelayRouter::Toggle()
{
	if (StreamMode())
		stream_.Toggle();
	else
		scene_.Toggle();
}

void DelayRouter::SkipToLive()
{
	Disable();
}

void DelayRouter::AdjustDelay(int deltaSeconds)
{
	if (StreamMode()) {
		stream_.AdjustDelay(deltaSeconds);
		return;
	}
	const int64_t next = static_cast<int64_t>(scene_.GetStatus().delaySeconds) + deltaSeconds;
	scene_.SetDelaySeconds(static_cast<uint32_t>(std::clamp<int64_t>(next, BufferModeController::kMinDelaySeconds,
										   BufferModeController::kMaxDelaySeconds)));
}

void DelayRouter::SetDelay(uint32_t seconds)
{
	if (StreamMode())
		stream_.SetDelaySeconds(seconds);
	else
		scene_.SetDelaySeconds(std::clamp<uint32_t>(seconds, BufferModeController::kMinDelaySeconds,
								   BufferModeController::kMaxDelaySeconds));
}

bool DelayRouter::Dump()
{
	return StreamMode() && stream_.Dump();
}

void DelayRouter::ApplyPreset(int index)
{
	if (StreamMode()) {
		stream_.ApplyPreset(index);
		return;
	}
	scene_.SetDelaySeconds(std::clamp<uint32_t>(stream_.Preset(index), BufferModeController::kMinDelaySeconds,
							   BufferModeController::kMaxDelaySeconds));
	scene_.Enable();
}

RouterStatus DelayRouter::Status() const
{
	RouterStatus out;
	if (StreamMode()) {
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

} // namespace trigglow
