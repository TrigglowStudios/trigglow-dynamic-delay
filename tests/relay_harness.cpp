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

// Manual end-to-end check of the stream mode relay (src/stream-relay.*),
// without OBS: not part of ctest (it needs FFmpeg's command line tools and
// opens local ports). Windows, with the plugin's FFmpeg.
//
//   relay_harness <destination rtmp url> <destination key> <script>
//
// It starts the relay with delay 0 and prints "PUBLISH <url> <key>" for the
// publisher (ffmpeg standing in for OBS). <script> is "t1:d1,t2:d2,...":
// at t seconds after start, set the delay to d. It prints the relay status
// every second and stops once the publisher ends and the destination finished.

#include "stream-relay.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "logging.hpp"

namespace trigglow::log {
void Log(Level level, const char *component, const char *fmt, ...)
{
	static const char *names[] = {"debug", "info", "warn", "error"};
	std::printf("[%s] [%s] ", names[static_cast<int>(level)], component);
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::printf("\n");
	std::fflush(stdout);
}
} // namespace trigglow::log

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::printf("usage: relay_harness <dest url> <dest key> <t:delay,...>\n");
		return 2;
	}
	std::vector<std::pair<int, int>> script;
	std::stringstream ss(argv[3]);
	std::string step;
	while (std::getline(ss, step, ',')) {
		const size_t colon = step.find(':');
		if (colon != std::string::npos)
			script.emplace_back(std::atoi(step.substr(0, colon).c_str()),
					    std::atoi(step.substr(colon + 1).c_str()));
	}

	trigglow::relay::StreamRelay relay;
	relay.SetEventCallback([](const std::string &m) { std::printf("EVENT %s\n", m.c_str()); });
	trigglow::relay::Destination dest;
	dest.name = "local";
	dest.server = argv[1];
	dest.key = argv[2];
	dest.primary = true;
	trigglow::relay::RelayOptions options;
	options.delaySeconds = 0;
	options.keepSeconds = 30;
	std::string server, key, error;
	if (!relay.Start({dest}, options, server, key, error)) {
		std::printf("START FAILED %s\n", error.c_str());
		return 1;
	}
	std::printf("PUBLISH %s %s\n", server.c_str(), key.c_str());
	std::fflush(stdout);

	size_t next = 0;
	for (int second = 0; second < 600; ++second) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		while (next < script.size() && script[next].first <= second + 1) {
			std::printf("SET DELAY %d at t=%d\n", script[next].second, second + 1);
			relay.SetDelay(static_cast<uint32_t>(script[next].second));
			++next;
		}
		const auto s = relay.GetStatus();
		const auto *sink = s.sinks.empty() ? nullptr : &s.sinks[0];
		std::printf(
			"t=%3d in=%d ended=%d delay=%u hist=%.1fs %.2fMB %.2fMbps kf=%.1fs %s %dx%d | sink state=%d "
			"behind=%.1fs sent=%.2fMbps bytes=%llu rc=%d rs=%d %s\n",
			second + 1, s.ingestConnected, s.ingestEnded, s.delaySeconds, s.historySeconds,
			s.historyBytes / 1048576.0, s.bitrateMbps, s.keyframeIntervalSeconds, s.videoCodec.c_str(),
			s.width, s.height, sink ? static_cast<int>(sink->state) : -1, sink ? sink->behindSeconds : 0.0,
			sink ? sink->sentMbps : 0.0, sink ? static_cast<unsigned long long>(sink->bytesSent) : 0ull,
			sink ? sink->reconnects : 0, sink ? sink->resyncs : 0, sink ? sink->error.c_str() : "");
		std::fflush(stdout);
		if (relay.Finished() && s.ingestEnded)
			break;
	}
	relay.Stop();
	std::printf("DONE\n");
	return 0;
}
