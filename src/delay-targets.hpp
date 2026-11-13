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

// The two delay modes as DelayRouter targets (delay-router.hpp): scene mode
// (all of OBS) and stream mode (only the stream). UI thread only.

#include "delay-router.hpp"

namespace trigglow {

class BufferModeController;
class StreamDelayController;

// All of OBS: scene mode. Seconds clamped to 1-600; its presets are stream
// mode's three (one set for both modes, saved in stream-mode-settings.json).
class SceneDelayTarget final : public DelayTarget {
public:
	SceneDelayTarget(BufferModeController &scene, const StreamDelayController &presets)
		: scene_(scene),
		  presets_(presets)
	{
	}
	void Enable() override;
	void Disable() override;
	void Toggle() override;
	void AdjustDelay(int deltaSeconds) override;
	void SetDelay(uint32_t seconds) override;
	bool Dump() override { return false; }
	void ApplyPreset(int index) override;
	RouterStatus Status() const override;
	bool SelectDelayedScene(const std::string &sceneName) override;
	bool SelectDelayedSceneSlot(size_t slot) override;

private:
	BufferModeController &scene_;
	const StreamDelayController &presets_;
};

// Only the stream: stream mode.
class StreamDelayTarget final : public DelayTarget {
public:
	explicit StreamDelayTarget(StreamDelayController &stream) : stream_(stream) {}
	void Enable() override;
	void Disable() override;
	void Toggle() override;
	void AdjustDelay(int deltaSeconds) override;
	void SetDelay(uint32_t seconds) override;
	bool Dump() override;
	void ApplyPreset(int index) override;
	RouterStatus Status() const override;

private:
	StreamDelayController &stream_;
};

} // namespace trigglow
