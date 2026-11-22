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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api-client.hpp"
#include "auth-client.hpp"
#include "json.hpp"
#include "stream-relay.hpp"

// v1.1.0, the link to Trigglow Multistream, without Qt or libobs (the plugin's
// side of it is multistream-service.hpp; tests/multistream_link_tests.cpp runs
// this file with a scripted transport):
//
//   - the streamer's Multistream channels as extra destinations of stream
//     mode's relay (GET /api/multistream/plugin/destinations);
//   - the delay's status for the dashboard, and the command the dashboard may
//     answer with (POST /api/multistream/dynamic-delay/status).
//
// Stream keys: a destination's key exists only in memory, in the
// relay::Destination this file hands back. Nothing here logs, and nothing of a
// destination but its name ever goes into a status report.
namespace trigglow::mslink {

inline constexpr const char *kDestinationsPath = "/api/multistream/plugin/destinations";
inline constexpr const char *kStatusPath = "/api/multistream/dynamic-delay/status";
// What the dock calls a stream sent to Trigglow's own Multistream ingest.
inline constexpr const char *kTrigglowIngestName = "Trigglow Multistream";
// The contract's limits: channels per report, and characters per name.
inline constexpr size_t kMaxChannels = 20;
inline constexpr size_t kMaxNameChars = 60;
// A cached list older than this is fetched again when OBS starts streaming.
inline constexpr int kCacheFreshSeconds = 60;

// --- Destinations ---------------------------------------------------------------

// A key of Trigglow's own Multistream ingest ("tgms_..."): its cloud relay
// already forwards to the streamer's channels.
bool IsTrigglowIngestKey(std::string_view key);

// At most kMaxNameChars characters of `name` (whole UTF-8 characters), without
// control characters.
std::string ClipName(std::string_view name);

// Same host (case-insensitive, whatever the scheme, port or path) and same key.
bool SameDestination(const relay::Destination &a, const relay::Destination &b);

// {"destinations":[{"name","server","key",...}]} as relay destinations (none
// primary). An entry without a name, with a server that isn't rtmp:// or
// rtmps://, or with an empty key is skipped; at most kMaxChannels are kept.
// nullopt when "destinations" is missing or not an array.
std::optional<std::vector<relay::Destination>> ParseDestinations(const json::Value &body);

struct ExtraDestinations {
	// What to add after the primary.
	std::vector<relay::Destination> extra;
	// The primary is Trigglow's ingest: nothing is added, the cloud forwards.
	bool cloudIngest = false;
};

// The fetched channels to send to besides `primary`: none when the primary's
// key is Trigglow's ingest; otherwise all of them but the one that is the
// primary itself and any repeated one (SameDestination).
ExtraDestinations PlanExtraDestinations(const relay::Destination &primary,
					const std::vector<relay::Destination> &fetched);

enum class FetchStatus : unsigned char {
	Ok,
	PlanRequired, // 403 plan_required: the account's plan can't send from the plugin.
	Unavailable,  // 404: Multistream is off for this account.
	RateLimited,  // 429.
	SignedOut,    // No token, or the server ended the session.
	Failed,       // Network, 5xx, an unexpected answer.
};

struct FetchResult {
	FetchStatus status = FetchStatus::Failed;
	std::vector<relay::Destination> destinations; // With Ok.
	auth::AuthEvents events;
};

// BLOCKING. GET kDestinationsPath through auth::CallAuthorized().
FetchResult FetchDestinations(const api::Client &client, TokenStore &store, const auth::Renewer *renewer = nullptr);

// --- Status and remote control ----------------------------------------------------

// "waiting", "connecting", "live", "reconnecting", "failed", "done".
const char *SinkStateName(relay::SinkState state);

struct Channel {
	std::string name;
	relay::SinkState state = relay::SinkState::Waiting;
};

struct StatusReport {
	bool active = false;  // The delay is on (filling included).
	uint32_t seconds = 0; // The configured delay.
	bool streamMode = false;
	bool streaming = false;        // OBS is streaming.
	std::vector<Channel> channels; // Stream mode's sinks; empty otherwise.
};

// The request body: seconds clamped to 0-600, at most kMaxChannels channels,
// names clipped (ClipName). Never a key or a URL: a Channel has neither.
json::Value StatusBody(const StatusReport &report, std::string_view version);

enum class CommandType : unsigned char { None, Enable, Disable, Dump, SetDelay };

struct Command {
	CommandType type = CommandType::None;
	uint32_t seconds = 0; // SetDelay: already within the plugin's range.
};

// A delay the dashboard asked for, within the plugin's own range (1-600 s).
uint32_t ClampCommandSeconds(double seconds);

// {"command":null} or {"command":{"type":"enable"|"disable"|"dump"}} or
// {"command":{"type":"set_delay","seconds":30}}. Anything else is None.
Command ParseCommand(const json::Value &body);

// Seconds between two reports: 5 while OBS streams or the delay is on, 30
// otherwise, 60 after kFailuresBeforeBackoff failed reports in a row.
inline constexpr int kFailuresBeforeBackoff = 3;
int HeartbeatIntervalSeconds(bool busy, int consecutiveFailures);

struct PostResult {
	bool ok = false;
	Command command;
	auth::AuthEvents events;
};

// BLOCKING. POST kStatusPath through auth::CallAuthorized().
PostResult PostStatus(const api::Client &client, TokenStore &store, const StatusReport &report,
		      const auth::Renewer *renewer = nullptr);

} // namespace trigglow::mslink
