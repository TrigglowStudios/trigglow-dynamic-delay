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

// v1.1.0: Aitum Vertical's stream outputs through stream mode's delay.
//
// With "Apply the delay only to the stream", the relay holds OBS's own stream
// output (stream-delay-controller.hpp). Aitum Vertical streams its canvas with
// outputs of its own ("vertical_canvas_stream[_<name>]", rtmp_output), which
// went out live. Aitum creates each output once (CreateStreamOutput) and from
// then on only updates its OWN service object before every start
// (obs_service_update(it->service, ...)); it calls obs_output_set_service()
// only when it creates the output. So this class gives each of those outputs
// a private loopback service pointing at a relay of its own (one
// StreamRelay per output, its destination the server and key Aitum's service
// holds), and the output keeps it across Aitum's stops and starts. Aitum's
// settings, dock and saved config never see the relay.
//
// The first start of an output in an OBS session creates it and starts it in
// the same call, so it goes out directly; the next update (at most half a
// second later, usually before its RTMP handshake is done) stops it through
// Aitum's own proc handler, takes it, and starts it again through the delay
// (aitum_vertical_stop_stream_output / _start_stream_output).
//
// Delay, Dump and the keep window follow stream mode's (StreamDelayController
// passes them); the badge, when it shows, is drawn on the Aitum Vertical
// canvas too (output channel 61, placed for that canvas). Only RTMP and RTMPS
// destinations; anything else stays as Aitum set it. UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stream-relay.hpp"

struct obs_output;
typedef struct obs_output obs_output_t;
struct obs_scene;
typedef struct obs_scene obs_scene_t;
struct obs_source;
typedef struct obs_source obs_source_t;

namespace trigglow {

class VerticalTaps {
public:
	struct Options {
		uint32_t delaySeconds = 0; // The effective delay now (0 when off).
		uint32_t keepSeconds = 60;
		bool drainOnStop = true;
		bool spillToDisk = false;
	};
	using EventCallback = std::function<void(const std::string &message)>;

	VerticalTaps();
	~VerticalTaps();
	VerticalTaps(const VerticalTaps &) = delete;
	VerticalTaps &operator=(const VerticalTaps &) = delete;

	// Relay events ("Aitum Vertical (host): ..."), from the relays' threads.
	void SetEventCallback(EventCallback callback) { onEvent_ = std::move(callback); }

	// About twice a second. `enabled`: "only the stream" is chosen and the
	// relay exists here; off, every output gets Aitum's service back as soon
	// as it isn't live (a live one keeps the delay until it stops).
	void Update(bool enabled, const Options &options);
	void SetDelay(uint32_t seconds);
	void SetKeepSeconds(uint32_t seconds);
	// What viewers haven't seen yet is never sent (StreamDelayController::Dump).
	void Dump();
	// OBS closing or the plugin unloading: every relay stops and Aitum gets
	// its services back.
	void Shutdown();

	// `output`'s service is one of these relays' (it is delayed). Safe inside
	// obs_enum_outputs (plain pointer reads).
	bool Owns(const obs_output_t *output) const;
	// An output goes out through one of the relays now, or its last seconds
	// are still being sent.
	bool AnyStreaming() const;
	// The destinations those relays send to, for the dock.
	std::vector<relay::SinkStatus> Sinks() const;

	// The badge on the Aitum Vertical canvas: `imagePath` (overlay-badge.hpp)
	// in `corner` while `visible` and an output goes through a relay.
	void SetBadge(bool visible, const std::string &imagePath, uint32_t corner);

private:
	struct Tap;

	void Step(Tap &tap, obs_output_t *output, bool enabled);
	bool StartRelay(Tap &tap, const std::string &server, const std::string &key);
	// Aitum's service back on the output when it isn't live; true when the
	// tap can go.
	bool GiveBack(Tap &tap, obs_output_t *output);
	void ReleaseTap(Tap &tap);
	void ClearBadge();

	std::vector<std::unique_ptr<Tap>> taps_;
	Options options_;
	EventCallback onEvent_;

	obs_scene_t *badgeScene_ = nullptr;
	obs_source_t *badgeSource_ = nullptr;
	std::string badgePath_;
	uint32_t badgeCorner_ = 99;
	uint32_t badgeWidth_ = 0;
	uint32_t badgeHeight_ = 0;
	bool badgeShown_ = false;
};

} // namespace trigglow
