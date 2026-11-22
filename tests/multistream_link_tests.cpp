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

// Tests for src/multistream-link.*: reading the Multistream channels, which of
// them stream mode sends to besides the streamer's own service, the status
// report, the dashboard's commands and how often the report goes out.

#include "multistream-link.hpp"
#include "fake-transport.hpp"

#include <memory>
#include <string>

using namespace trigglow;
using testsupport::FakeTransport;
using testsupport::HeaderValue;
using testsupport::Json;
using testsupport::ScratchDir;
using testsupport::Url;

namespace {

class FakeProtector final : public SecretProtector {
public:
	bool Available() const override { return true; }
	bool Protect(const std::string &plain, std::string &sealed) const override
	{
		sealed = "F" + plain;
		return true;
	}
	bool Unprotect(const std::string &sealed, std::string &plain) const override
	{
		if (sealed.empty() || sealed[0] != 'F')
			return false;
		plain = sealed.substr(1);
		return true;
	}
};

struct Fixture {
	ScratchDir dir{"mslink"};
	FakeTransport transport;
	api::Client client{transport.Fn(), "api.trigglow.com", "1.1.0"};
	TokenStore store{dir.path() / "trigglow-auth.dat", dir.path() / "trigglow-account.json",
			 std::make_unique<FakeProtector>()};

	void SignIn() { store.Save(PluginToken{"tok_abc", "2999-02-01T00:00:00Z", "2999-01-01T00:00:00Z", "Name"}); }
};

relay::Destination Dest(const std::string &name, const std::string &server, const std::string &key,
			bool primary = false)
{
	relay::Destination d;
	d.name = name;
	d.server = server;
	d.key = key;
	d.primary = primary;
	return d;
}

std::string DestJson(const std::string &name, const std::string &server, const std::string &key)
{
	return "{\"id\":\"0b8f6f0e-1111-4222-8333-444455556666\",\"name\":\"" + name +
	       "\",\"platform\":\"x\",\"server\":\"" + server + "\",\"key\":\"" + key + "\"}";
}

const std::string kKick = Url("rtmps", "ingest.kick.example/app");
const std::string kTwitch = Url("rtmp", "live.twitch.example/app");

void TestParseDestinations()
{
	testcheck::g_currentTest = "parse destinations";
	const auto parsed =
		mslink::ParseDestinations(*json::Parse("{\"destinations\":[" + DestJson("Kick", kKick, "k1") + "," +
						       DestJson("Twitch", kTwitch, "k2") + "]}"));
	CHECK(parsed.has_value());
	CHECK_EQ(parsed->size(), 2u);
	CHECK_STR((*parsed)[0].name, "Kick");
	CHECK_STR((*parsed)[0].server, kKick);
	CHECK_STR((*parsed)[0].key, "k1");
	CHECK(!(*parsed)[0].primary);
	CHECK_STR((*parsed)[1].name, "Twitch");

	// An empty list is a valid answer; a missing or wrong one isn't.
	CHECK(mslink::ParseDestinations(*json::Parse("{\"destinations\":[]}"))->empty());
	CHECK(!mslink::ParseDestinations(*json::Parse("{}")).has_value());
	CHECK(!mslink::ParseDestinations(*json::Parse("{\"destinations\":{}}")).has_value());
	CHECK(!mslink::ParseDestinations(*json::Parse("[]")).has_value());

	// Entries the relay couldn't use are skipped, the rest kept.
	const auto mixed = mslink::ParseDestinations(
		*json::Parse("{\"destinations\":[" + DestJson("", kKick, "k") + "," + DestJson("NoKey", kKick, "") +
			     "," + DestJson("Srt", Url("srt", "host.example:9000"), "k") + "," +
			     DestJson("Https", Url("https", "x.example"), "k") +
			     ",42,{\"name\":\"NoServer\",\"key\":\"k\"}," + DestJson("Ok", kTwitch, "k3") + "]}"));
	CHECK(mixed.has_value());
	CHECK_EQ(mixed->size(), 1u);
	CHECK_STR((*mixed)[0].name, "Ok");

	// At most 20, names at most 60 characters.
	std::string many = "{\"destinations\":[";
	for (int i = 0; i < 30; ++i)
		many += (i ? "," : "") + DestJson("C" + std::to_string(i), kKick, "k" + std::to_string(i));
	many += "]}";
	CHECK_EQ(mslink::ParseDestinations(*json::Parse(many))->size(), mslink::kMaxChannels);
	const auto longName = mslink::ParseDestinations(
		*json::Parse("{\"destinations\":[" + DestJson(std::string(80, 'a'), kKick, "k") + "]}"));
	CHECK_EQ((*longName)[0].name.size(), 60u);
}

void TestClipName()
{
	testcheck::g_currentTest = "clip name";
	CHECK_STR(mslink::ClipName("Twitch"), "Twitch");
	CHECK_EQ(mslink::ClipName(std::string(61, 'x')).size(), 60u);
	// Whole UTF-8 characters: 61 two-byte letters become 60 (120 bytes).
	std::string accents;
	for (int i = 0; i < 61; ++i)
		accents += "\xC3\xA9";
	CHECK_EQ(mslink::ClipName(accents).size(), 120u);
	// A truncated sequence at the end doesn't read past the text.
	CHECK_STR(mslink::ClipName("a\xE4\xB8"), "a\xE4\xB8");
	// Control characters are dropped.
	CHECK_STR(mslink::ClipName("Ki\nck\t"), "Kick");
}

void TestPlan()
{
	testcheck::g_currentTest = "plan";
	const relay::Destination primary = Dest("Twitch", Url("rtmp", "live.twitch.example/app"), "pk", true);
	const std::vector<relay::Destination> fetched = {
		Dest("Kick", kKick, "k1"),
		// The primary itself: same host (case and port aside) and key.
		Dest("Twitch", Url("rtmps", "LIVE.twitch.example:443/app2"), "pk"),
		// Same host as the primary, another key: another channel.
		Dest("Twitch 2", kTwitch, "other"),
		// Kick again.
		Dest("Kick copy", kKick, "k1"),
		// Same key on another host: kept.
		Dest("Elsewhere", Url("rtmp", "other.example/live"), "k1"),
	};
	const mslink::ExtraDestinations plan = mslink::PlanExtraDestinations(primary, fetched);
	CHECK(!plan.cloudIngest);
	CHECK_EQ(plan.extra.size(), 3u);
	CHECK_STR(plan.extra[0].name, "Kick");
	CHECK_STR(plan.extra[1].name, "Twitch 2");
	CHECK_STR(plan.extra[2].name, "Elsewhere");
	for (const relay::Destination &d : plan.extra)
		CHECK(!d.primary);

	CHECK(mslink::PlanExtraDestinations(primary, {}).extra.empty());

	// Trigglow's own ingest already forwards to the channels: none added.
	CHECK(mslink::IsTrigglowIngestKey("tgms_abc"));
	CHECK(!mslink::IsTrigglowIngestKey("live_tgms_abc"));
	CHECK(!mslink::IsTrigglowIngestKey("tgms"));
	CHECK(!mslink::IsTrigglowIngestKey(""));
	const mslink::ExtraDestinations cloud = mslink::PlanExtraDestinations(
		Dest("x", Url("rtmp", "ingest.trigglow.example/live"), "tgms_abc", true), fetched);
	CHECK(cloud.cloudIngest);
	CHECK(cloud.extra.empty());

	CHECK(mslink::SameDestination(Dest("a", Url("rtmp", "h.example/app"), "k"), Dest("b", "H.EXAMPLE", "k")));
	CHECK(!mslink::SameDestination(Dest("a", Url("rtmp", "h.example/app"), "k"),
				       Dest("a", Url("rtmp", "h.example/app"), "K")));
}

void TestFetch()
{
	testcheck::g_currentTest = "fetch";
	{
		Fixture f;
		// Signed out: no request at all.
		const mslink::FetchResult r = mslink::FetchDestinations(f.client, f.store);
		CHECK(r.status == mslink::FetchStatus::SignedOut);
		CHECK_EQ(f.transport.Count(), 0u);
	}
	{
		Fixture f;
		f.SignIn();
		f.transport.Push(Json(200, "{\"destinations\":[" + DestJson("Kick", kKick, "k1") + "]}"));
		const mslink::FetchResult r = mslink::FetchDestinations(f.client, f.store);
		CHECK(r.status == mslink::FetchStatus::Ok);
		CHECK_EQ(r.destinations.size(), 1u);
		const http::Request req = f.transport.Last();
		CHECK_STR(req.method, "GET");
		CHECK_STR(req.host, "api.trigglow.com");
		CHECK_STR(req.path, "/api/multistream/plugin/destinations");
		CHECK_STR(HeaderValue(req, "Authorization"), "Bearer tok_abc");
	}
	const struct {
		int status;
		const char *body;
		mslink::FetchStatus expected;
	} cases[] = {
		{403, "{\"error\":\"plan_required\"}", mslink::FetchStatus::PlanRequired},
		{403, "{\"error\":\"something_else\"}", mslink::FetchStatus::Failed},
		{404, "{\"error\":\"multistream_unavailable\"}", mslink::FetchStatus::Unavailable},
		{429, "{\"error\":\"rate_limited\"}", mslink::FetchStatus::RateLimited},
		{500, "{}", mslink::FetchStatus::Failed},
		{200, "{\"nothing\":true}", mslink::FetchStatus::Failed},
		{200, "not json", mslink::FetchStatus::Failed},
	};
	for (const auto &c : cases) {
		Fixture f;
		f.SignIn();
		f.transport.Push(Json(c.status, c.body));
		const mslink::FetchResult r = mslink::FetchDestinations(f.client, f.store);
		CHECK(r.status == c.expected);
		CHECK(r.destinations.empty());
		CHECK_EQ(f.transport.Count(), 1u); // Never retried.
	}
	{
		// The server ended the session: the sign-in is cleared like for any call.
		Fixture f;
		f.SignIn();
		f.transport.Push(Json(401, "{\"error\":\"invalid_token\"}"));
		const mslink::FetchResult r = mslink::FetchDestinations(f.client, f.store);
		CHECK(r.status == mslink::FetchStatus::SignedOut);
		CHECK(r.events.sessionEnded);
		CHECK(!f.store.Current().has_value());
	}
	{
		Fixture f;
		f.SignIn();
		f.transport.Push(testsupport::NetworkDown());
		CHECK(mslink::FetchDestinations(f.client, f.store).status == mslink::FetchStatus::Failed);
	}
}

void TestStatusBody()
{
	testcheck::g_currentTest = "status body";
	mslink::StatusReport report;
	report.active = true;
	report.seconds = 30;
	report.streamMode = true;
	report.streaming = true;
	report.channels = {{"Twitch", relay::SinkState::Live}, {"Kick", relay::SinkState::Reconnecting}};
	CHECK_STR(json::Serialize(mslink::StatusBody(report, "1.1.0")),
		  "{\"active\":true,\"seconds\":30,\"mode\":\"stream\",\"streaming\":true,\"channels\":["
		  "{\"name\":\"Twitch\",\"state\":\"live\"},{\"name\":\"Kick\",\"state\":\"reconnecting\"}],"
		  "\"version\":\"1.1.0\"}");

	mslink::StatusReport idle;
	idle.seconds = 5000; // Never above the contract's 600.
	CHECK_STR(json::Serialize(mslink::StatusBody(idle, "1.1.0")),
		  "{\"active\":false,\"seconds\":600,\"mode\":\"all\",\"streaming\":false,\"channels\":[],"
		  "\"version\":\"1.1.0\"}");

	CHECK_STR(mslink::SinkStateName(relay::SinkState::Waiting), "waiting");
	CHECK_STR(mslink::SinkStateName(relay::SinkState::Connecting), "connecting");
	CHECK_STR(mslink::SinkStateName(relay::SinkState::Failed), "failed");
	CHECK_STR(mslink::SinkStateName(relay::SinkState::Done), "done");

	// At most 20 channels, names at most 60 characters.
	mslink::StatusReport big;
	for (int i = 0; i < 25; ++i)
		big.channels.push_back({std::string(70, 'n'), relay::SinkState::Waiting});
	const json::Value body = mslink::StatusBody(big, "1.1.0");
	CHECK_EQ(body.Find("channels")->Size(), 20u);
	CHECK_EQ(body.Find("channels")->At(0).GetString("name")->size(), 60u);
}

void TestCommands()
{
	testcheck::g_currentTest = "commands";
	auto parse = [](const char *text) {
		return mslink::ParseCommand(*json::Parse(text));
	};
	CHECK(parse("{\"command\":null}").type == mslink::CommandType::None);
	CHECK(parse("{}").type == mslink::CommandType::None);
	CHECK(parse("{\"command\":\"enable\"}").type == mslink::CommandType::None);
	CHECK(parse("{\"command\":{\"type\":\"enable\"}}").type == mslink::CommandType::Enable);
	CHECK(parse("{\"command\":{\"type\":\"disable\"}}").type == mslink::CommandType::Disable);
	CHECK(parse("{\"command\":{\"type\":\"dump\"}}").type == mslink::CommandType::Dump);
	CHECK(parse("{\"command\":{\"type\":\"reboot\"}}").type == mslink::CommandType::None);
	CHECK(parse("{\"command\":{\"type\":42}}").type == mslink::CommandType::None);

	const mslink::Command set = parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":30}}");
	CHECK(set.type == mslink::CommandType::SetDelay);
	CHECK_EQ(set.seconds, 30u);
	// Clamped to the plugin's own range.
	CHECK_EQ(parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":0}}").seconds, 1u);
	CHECK_EQ(parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":-5}}").seconds, 1u);
	CHECK_EQ(parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":100000}}").seconds, 600u);
	CHECK_EQ(parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":12.6}}").seconds, 13u);
	// Without its seconds it is no command.
	CHECK(parse("{\"command\":{\"type\":\"set_delay\"}}").type == mslink::CommandType::None);
	CHECK(parse("{\"command\":{\"type\":\"set_delay\",\"seconds\":\"30\"}}").type == mslink::CommandType::None);

	CHECK_EQ(mslink::ClampCommandSeconds(1), 1u);
	CHECK_EQ(mslink::ClampCommandSeconds(600), 600u);
	CHECK_EQ(mslink::ClampCommandSeconds(600.4), 600u);
	CHECK_EQ(mslink::ClampCommandSeconds(1e300), 600u);
	CHECK_EQ(mslink::ClampCommandSeconds(std::nan("")), 1u);
}

void TestHeartbeat()
{
	testcheck::g_currentTest = "heartbeat";
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(true, 0), 5);
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(false, 0), 30);
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(true, 2), 5);
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(false, 2), 30);
	// Repeated failures: once a minute, busy or not.
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(true, 3), 60);
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(false, 3), 60);
	CHECK_EQ(mslink::HeartbeatIntervalSeconds(true, 100), 60);
}

void TestPostStatus()
{
	testcheck::g_currentTest = "post status";
	mslink::StatusReport report;
	report.active = true;
	report.seconds = 20;
	report.streaming = true;
	{
		Fixture f;
		CHECK(!mslink::PostStatus(f.client, f.store, report).ok); // Signed out.
		CHECK_EQ(f.transport.Count(), 0u);
	}
	{
		Fixture f;
		f.SignIn();
		f.transport.Push(Json(200, "{\"command\":{\"type\":\"set_delay\",\"seconds\":45}}"));
		const mslink::PostResult r = mslink::PostStatus(f.client, f.store, report);
		CHECK(r.ok);
		CHECK(r.command.type == mslink::CommandType::SetDelay);
		CHECK_EQ(r.command.seconds, 45u);
		const http::Request req = f.transport.Last();
		CHECK_STR(req.method, "POST");
		CHECK_STR(req.path, "/api/multistream/dynamic-delay/status");
		CHECK_STR(HeaderValue(req, "Authorization"), "Bearer tok_abc");
		// The client's own version, never a key or a URL.
		CHECK_STR(req.body,
			  "{\"active\":true,\"seconds\":20,\"mode\":\"all\",\"streaming\":true,\"channels\":[],"
			  "\"version\":\"1.1.0\"}");
	}
	{
		Fixture f;
		f.SignIn();
		f.transport.Push(Json(200, "{\"command\":null}"));
		const mslink::PostResult r = mslink::PostStatus(f.client, f.store, report);
		CHECK(r.ok);
		CHECK(r.command.type == mslink::CommandType::None);
	}
	for (int status : {404, 429, 500}) {
		Fixture f;
		f.SignIn();
		// A command in a failed answer is never applied.
		f.transport.Push(Json(status, "{\"command\":{\"type\":\"enable\"}}"));
		const mslink::PostResult r = mslink::PostStatus(f.client, f.store, report);
		CHECK(!r.ok);
		CHECK(r.command.type == mslink::CommandType::None);
		CHECK_EQ(f.transport.Count(), 1u);
	}
}

} // namespace

int main()
{
	TestParseDestinations();
	TestClipName();
	TestPlan();
	TestFetch();
	TestStatusBody();
	TestCommands();
	TestHeartbeat();
	TestPostStatus();
	return testcheck::Finish("multistream_link_tests");
}
