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
//   relay_harness <destination rtmp url> <destination key> <script> [keep s] [max s]
//
// It starts the relay with delay 0 and prints "PUBLISH <url> <key>" for the
// publisher (ffmpeg standing in for OBS). <script> is "t1:d1,t2:d2,...":
// t seconds after OBS's stream reached the relay, set the delay to d (-1:
// Dump). The steps run half a second off the stream's whole seconds, so two
// runs of the same input splice at the same keyframes. It prints the relay
// status and this process's memory every second and stops once the publisher
// ends and the destination finished, or after [max s] (default 600). [keep s]
// is the relay's keepSeconds (default 30).

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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

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

namespace {

// This process's private bytes and working set, in MB (0 where unsupported).
void ProcessMemoryMb(double &privateMb, double &workingSetMb)
{
	privateMb = 0.0;
	workingSetMb = 0.0;
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
				 sizeof(counters))) {
		privateMb = static_cast<double>(counters.PrivateUsage) / 1048576.0;
		workingSetMb = static_cast<double>(counters.WorkingSetSize) / 1048576.0;
	}
#endif
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::printf("usage: relay_harness <dest url> <dest key> <t:delay,...> [keep s] [max s]\n");
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
	const int keepSeconds = argc > 4 ? std::atoi(argv[4]) : 30;
	const int maxSeconds = argc > 5 ? std::atoi(argv[5]) : 600;

	trigglow::relay::StreamRelay relay;
	relay.SetEventCallback([](const std::string &m) { std::printf("EVENT %s\n", m.c_str()); });
	trigglow::relay::Destination dest;
	dest.name = "local";
	dest.server = argv[1];
	dest.key = argv[2];
	dest.primary = true;
	trigglow::relay::RelayOptions options;
	options.delaySeconds = 0;
	options.keepSeconds = static_cast<uint32_t>(keepSeconds);
	std::string server, key, error;
	if (!relay.Start({dest}, options, server, key, error)) {
		std::printf("START FAILED %s\n", error.c_str());
		return 1;
	}
	std::printf("PUBLISH %s %s\n", server.c_str(), key.c_str());
	std::fflush(stdout);

	// The script's clock starts when OBS's stream reaches the relay.
	using Clock = std::chrono::steady_clock;
	const Clock::time_point started = Clock::now();
	while (!relay.GetStatus().ingestConnected && !relay.Finished() &&
	       Clock::now() - started < std::chrono::seconds(maxSeconds))
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
	std::printf("INGEST after %lld ms\n", static_cast<long long>(waitedMs));
	const Clock::time_point t0 = Clock::now() + std::chrono::milliseconds(500);

	size_t next = 0;
	for (int second = 0; second < maxSeconds; ++second) {
		std::this_thread::sleep_until(t0 + std::chrono::seconds(second));
		while (next < script.size() && script[next].first <= second) {
			if (script[next].second < 0) {
				std::printf("DUMP at t=%d\n", second);
				relay.Dump();
			} else {
				std::printf("SET DELAY %d at t=%d\n", script[next].second, second);
				relay.SetDelay(static_cast<uint32_t>(script[next].second));
			}
			++next;
		}
		const auto s = relay.GetStatus();
		const auto *sink = s.sinks.empty() ? nullptr : &s.sinks[0];
		double privateMb = 0.0;
		double workingSetMb = 0.0;
		ProcessMemoryMb(privateMb, workingSetMb);
		std::printf(
			"t=%3d in=%d ended=%d delay=%u hist=%.1fs %.2fMB hram=%.2fMB %.2fMbps kf=%.1fs %s %dx%d | priv=%.1fMB "
			"ws=%.1fMB | sink state=%d behind=%.1fs sent=%.2fMbps bytes=%llu rc=%d rs=%d %s\n",
			second, s.ingestConnected, s.ingestEnded, s.delaySeconds, s.historySeconds,
			s.historyBytes / 1048576.0, s.historyRamBytes / 1048576.0, s.bitrateMbps,
			s.keyframeIntervalSeconds, s.videoCodec.c_str(), s.width, s.height, privateMb, workingSetMb,
			sink ? static_cast<int>(sink->state) : -1, sink ? sink->behindSeconds : 0.0,
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
