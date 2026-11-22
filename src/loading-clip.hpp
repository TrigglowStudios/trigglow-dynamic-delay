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

// v1.1.0 loading screen in stream mode.
//
// Stream mode delays packets OBS has already encoded, so what viewers see
// while the delay builds up can't come from OBS's stream: raising the delay
// used to replay the last seconds. This makes a short clip of the loading
// scene (Trigglow's "adding delay" video unless the plan lets the streamer
// choose another, settings-ui.cpp) encoded exactly like the stream: for each
// video encoder of OBS's stream output, a private canvas at that encoder's
// size and frame rate shows the loading scene fitted inside, and a copy of
// the encoder (same id and settings) encodes it into a capture output for
// one loop (8 s: the default video's own length, so the loop is seamless).
// The relay plays it, looped, with silence (stream-relay.hpp
// SetLoadingClip). Everything is released once the clip is captured, so the
// extra encoding only lasts those seconds. UI thread only.

#include "stream-relay.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct obs_output;
typedef struct obs_output obs_output_t;

namespace trigglow {

class LoadingClipMaker {
public:
	enum class State { Idle, Running, Done, Failed };

	LoadingClipMaker();
	~LoadingClipMaker();
	LoadingClipMaker(const LoadingClipMaker &) = delete;
	LoadingClipMaker &operator=(const LoadingClipMaker &) = delete;

	// Starts encoding the scene `sceneName` like every video encoder of
	// `output`. False, with why, when it can't (no such scene, no encoder).
	bool Start(obs_output_t *output, const std::string &sceneName, int64_t loopMs, std::string &why);
	// Twice a second: Done hands the clip over (once), Failed says why.
	State Poll(std::vector<relay::LoadingVideoTrack> &tracksOut, std::string &why);
	// Stops and releases everything (a stream that ended, OBS closing).
	void Cancel();
	bool Running() const { return state_ == State::Running; }

private:
	struct Track;
	void Release();

	std::vector<std::unique_ptr<Track>> tracks_;
	State state_ = State::Idle;
	int64_t loopMs_ = 8000;
	int64_t startedNs_ = 0;
};

} // namespace trigglow
