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

#include "multistream-link.hpp"

#include <algorithm>
#include <cmath>

#include "packet-delay-core.hpp"

namespace trigglow::mslink {

namespace {

bool StartsWith(std::string_view text, std::string_view prefix)
{
	return text.substr(0, prefix.size()) == prefix;
}

// "rtmps://Host:443/app" -> "host". stream-relay.cpp's HostOf() in lower case;
// its own copy so this file links without the relay.
std::string HostKey(std::string_view url)
{
	size_t start = url.find("://");
	start = start == std::string_view::npos ? 0 : start + 3;
	const size_t end = url.find_first_of(":/?", start);
	std::string host(url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
	for (char &c : host) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return host;
}

} // namespace

bool IsTrigglowIngestKey(std::string_view key)
{
	return StartsWith(key, "tgms_");
}

std::string ClipName(std::string_view name)
{
	std::string out;
	size_t chars = 0;
	for (size_t i = 0; i < name.size() && chars < kMaxNameChars;) {
		const unsigned char lead = static_cast<unsigned char>(name[i]);
		size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
		length = std::min(length, name.size() - i);
		if (lead >= 0x20 && lead != 0x7F) {
			out.append(name.substr(i, length));
			++chars;
		}
		i += length;
	}
	return out;
}

bool SameDestination(const relay::Destination &a, const relay::Destination &b)
{
	return a.key == b.key && HostKey(a.server) == HostKey(b.server);
}

std::optional<std::vector<relay::Destination>> ParseDestinations(const json::Value &body)
{
	const json::Value *list = body.Find("destinations");
	if (!list || !list->IsArray())
		return std::nullopt;
	std::vector<relay::Destination> out;
	for (size_t i = 0; i < list->Size() && out.size() < kMaxChannels; ++i) {
		const json::Value &item = list->At(i);
		relay::Destination destination;
		destination.name = ClipName(item.GetString("name").value_or(""));
		destination.server = item.GetString("server").value_or("");
		destination.key = item.GetString("key").value_or("");
		if (destination.name.empty() || destination.key.empty() ||
		    !(StartsWith(destination.server, "rtmp://") || StartsWith(destination.server, "rtmps://")))
			continue;
		out.push_back(std::move(destination));
	}
	return out;
}

ExtraDestinations PlanExtraDestinations(const relay::Destination &primary,
					const std::vector<relay::Destination> &fetched)
{
	ExtraDestinations plan;
	if (IsTrigglowIngestKey(primary.key)) {
		plan.cloudIngest = true;
		return plan;
	}
	for (const relay::Destination &candidate : fetched) {
		if (plan.extra.size() >= kMaxChannels)
			break;
		if (SameDestination(candidate, primary))
			continue;
		const bool repeated = std::any_of(plan.extra.begin(), plan.extra.end(),
						  [&candidate](const relay::Destination &added) {
							  return SameDestination(candidate, added);
						  });
		if (repeated)
			continue;
		plan.extra.push_back(candidate);
		plan.extra.back().primary = false;
	}
	return plan;
}

FetchResult FetchDestinations(const api::Client &client, TokenStore &store, const auth::Renewer *renewer)
{
	FetchResult out;
	const api::Result r = auth::CallAuthorized(
		client, store, [&client](const std::string &token) { return client.Get(kDestinationsPath, token); },
		out.events, std::time(nullptr), true, renewer);
	if (r.Ok()) {
		std::optional<std::vector<relay::Destination>> parsed;
		if (r.body)
			parsed = ParseDestinations(*r.body);
		if (parsed) {
			out.status = FetchStatus::Ok;
			out.destinations = std::move(*parsed);
		}
	} else if (r.Is(403, "plan_required")) {
		out.status = FetchStatus::PlanRequired;
	} else if (r.Is(404)) {
		out.status = FetchStatus::Unavailable;
	} else if (r.Is(429)) {
		out.status = FetchStatus::RateLimited;
	} else if (r.Is(401) || out.events.sessionEnded) {
		out.status = FetchStatus::SignedOut;
	}
	return out;
}

const char *SinkStateName(relay::SinkState state)
{
	switch (state) {
	case relay::SinkState::Waiting:
		return "waiting";
	case relay::SinkState::Connecting:
		return "connecting";
	case relay::SinkState::Live:
		return "live";
	case relay::SinkState::Reconnecting:
		return "reconnecting";
	case relay::SinkState::Failed:
		return "failed";
	case relay::SinkState::Done:
		return "done";
	}
	return "waiting";
}

json::Value StatusBody(const StatusReport &report, std::string_view version)
{
	json::Value channels = json::Value::Array();
	for (size_t i = 0; i < report.channels.size() && i < kMaxChannels; ++i) {
		json::Value channel = json::Value::Object();
		channel.Set("name", json::Value::String(ClipName(report.channels[i].name)));
		channel.Set("state", json::Value::String(SinkStateName(report.channels[i].state)));
		channels.Push(std::move(channel));
	}
	json::Value body = json::Value::Object();
	body.Set("active", json::Value::Bool(report.active));
	body.Set("seconds", json::Value::Number(std::min(report.seconds, relay::kMaxDelaySeconds)));
	body.Set("mode", json::Value::String(report.streamMode ? "stream" : "all"));
	body.Set("streaming", json::Value::Bool(report.streaming));
	body.Set("channels", std::move(channels));
	body.Set("version", json::Value::String(std::string(version)));
	return body;
}

uint32_t ClampCommandSeconds(double seconds)
{
	if (!(seconds >= 1.0)) // Also NaN.
		return 1;
	if (seconds >= static_cast<double>(relay::kMaxDelaySeconds))
		return relay::kMaxDelaySeconds;
	return static_cast<uint32_t>(std::lround(seconds));
}

Command ParseCommand(const json::Value &body)
{
	Command out;
	const json::Value *command = body.Find("command");
	if (!command || !command->IsObject())
		return out;
	const std::string type = command->GetString("type").value_or("");
	if (type == "enable") {
		out.type = CommandType::Enable;
	} else if (type == "disable") {
		out.type = CommandType::Disable;
	} else if (type == "dump") {
		out.type = CommandType::Dump;
	} else if (type == "set_delay") {
		if (const std::optional<double> seconds = command->GetNumber("seconds")) {
			out.type = CommandType::SetDelay;
			out.seconds = ClampCommandSeconds(*seconds);
		}
	}
	return out;
}

int HeartbeatIntervalSeconds(bool busy, int consecutiveFailures)
{
	if (consecutiveFailures >= kFailuresBeforeBackoff)
		return 60;
	return busy ? 5 : 30;
}

PostResult PostStatus(const api::Client &client, TokenStore &store, const StatusReport &report,
		      const auth::Renewer *renewer)
{
	PostResult out;
	const json::Value body = StatusBody(report, api::ClientVersion(client.PluginVersion()));
	const api::Result r = auth::CallAuthorized(
		client, store,
		[&client, &body](const std::string &token) {
			return client.PostJson(kStatusPath, body, token, api::Timeout::Short);
		},
		out.events, std::time(nullptr), true, renewer);
	if (!r.Ok())
		return out;
	out.ok = true;
	if (r.body)
		out.command = ParseCommand(*r.body);
	return out;
}

} // namespace trigglow::mslink
