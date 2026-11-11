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

// v1.0.0 stream mode: the relay. OBS's own stream output publishes to it on
// 127.0.0.1 (StreamDelayController points the output there for that stream
// only; the streamer's service settings are never changed). The relay keeps
// the encoded packets (packet-delay-core.hpp) and sends them on to every
// destination -- the streamer's own service plus any extra ones (multistream)
// -- each after the delay, through FFmpeg's RTMP/RTMPS client. Nothing is
// decoded or encoded again.
//
// Threads: one reads OBS's stream, one per destination writes. All of them
// stop and are joined in Stop() / the destructor. No OBS or Qt call is made
// from them; the controller polls GetStatus() and receives events through the
// callback (which it must marshal to its own thread).
//
// Available only where the plugin ships FFmpeg's avformat (Windows for now):
// RelayAvailable() is false elsewhere and every call fails cleanly.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace trigglow::relay {

bool RelayAvailable();

struct Destination {
	std::string name;     // Shown in the dock ("Twitch", "Kick"...). Never the key.
	std::string server;   // rtmp:// or rtmps:// URL, with the app path.
	std::string key;      // Stream key. Never logged.
	bool primary = false; // The streamer's own OBS service.
};

enum class SinkState { Waiting, Connecting, Live, Reconnecting, Failed, Done };

struct SinkStatus {
	std::string name;
	bool primary = false;
	SinkState state = SinkState::Waiting;
	std::string error; // Short, never contains the key.
	double sentMbps = 0.0;
	uint64_t bytesSent = 0;
	double behindSeconds = 0.0; // Content age of what this destination is sending now.
	int reconnects = 0;
	int resyncs = 0; // Times it fell behind and jumped to the delay again.
};

struct RelayStatus {
	bool running = false;
	bool ingestConnected = false;
	bool ingestEnded = false; // OBS stopped sending; destinations finish the delay.
	uint32_t delaySeconds = 0;
	double historySeconds = 0.0;
	uint64_t historyBytes = 0;
	double bitrateMbps = 0.0;
	double keyframeIntervalSeconds = 0.0;
	std::string videoCodec;
	int width = 0;
	int height = 0;
	std::vector<SinkStatus> sinks;
};

struct RelayOptions {
	uint32_t delaySeconds = 0;
	// The history always reaches back at least this far, so the delay can be
	// raised live up to it with no wait (packet-delay-core.hpp RetentionNs).
	uint32_t keepSeconds = 120;
	// When OBS stops streaming with the delay on, keep sending what viewers
	// haven't seen yet (the last N seconds) before closing.
	bool drainOnStop = true;
};

class StreamRelay {
public:
	using EventCallback = std::function<void(const std::string &message)>;

	StreamRelay();
	~StreamRelay();
	StreamRelay(const StreamRelay &) = delete;
	StreamRelay &operator=(const StreamRelay &) = delete;

	// Starts listening on 127.0.0.1 and returns where OBS must publish
	// (server URL and stream key). False with a short reason on failure.
	bool Start(const std::vector<Destination> &destinations, const RelayOptions &options, std::string &serverOut,
		   std::string &keyOut, std::string &errorOut);

	// Live delay change: every destination splices at once.
	void SetDelay(uint32_t seconds);
	uint32_t Delay() const;

	// Cuts everything now (OBS closing, a new stream starting).
	void Stop();

	bool Running() const;
	// True once OBS has stopped sending and every destination has finished.
	bool Finished() const;

	RelayStatus GetStatus() const;
	// Raised from the relay's threads for things the streamer should see
	// (a destination failed, reconnected...). Set before Start().
	void SetEventCallback(EventCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// "rtmps://host:443/app/" -> "host" for the dock and the log. Never the key.
std::string HostOf(const std::string &url);

} // namespace trigglow::relay
