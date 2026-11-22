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

#include "multistream-service.hpp"

#include <QTimer>

#include <memory>

extern "C" {
#include <obs-frontend-api.h>
}

#include "auth-manager.hpp"
#include "delay-router.hpp"
#include "logging.hpp"
#include "stream-delay-controller.hpp"
#include "ui-tasks.hpp"
#include "win-http.hpp"

namespace trigglow {

namespace {

constexpr const char *kComponent = "multistream";
// A failed or rate-limited fetch is tried again after this long (the server
// allows 20 a minute per account).
constexpr std::chrono::seconds kFetchRetry{60};
// A change of state is reported about this long after the last one.
constexpr std::chrono::seconds kChangeDebounce{1};

// The keys don't stay in freed memory.
void Wipe(std::vector<relay::Destination> &channels)
{
	for (relay::Destination &channel : channels) {
		volatile char *p = channel.key.data();
		for (size_t i = 0; i < channel.key.size(); ++i)
			p[i] = 0;
	}
	channels.clear();
}

} // namespace

MultistreamLink::MultistreamLink(AuthManager &auth, StreamDelayController &stream, DelayRouter &router)
	: auth_(auth),
	  stream_(stream),
	  router_(router),
	  tasks_(uitasks::Registry())
{
}

MultistreamLink::~MultistreamLink()
{
	Shutdown();
}

void MultistreamLink::Init()
{
	if (timer_)
		return;
	wasSignedIn_ = auth_.HasPluginToken();
	connect(&auth_, &AuthManager::StatusChanged, this, [this] { OnAuthChanged(); });
	timer_ = new QTimer(this);
	timer_->setInterval(1000);
	connect(timer_, &QTimer::timeout, this, [this] { Tick(); });
	timer_->start();
	Refresh();
}

void MultistreamLink::Shutdown()
{
	if (timer_)
		timer_->stop();
	disconnect(&auth_, nullptr, this, nullptr);
	tasks_.CancelAndWait(tasks::kOwnerLimit);
	onChanged_ = nullptr;
	ClearChannels();
}

void MultistreamLink::SetEnabled(bool enabled)
{
	stream_.SetSendToMultistream(enabled);
	if (enabled) {
		if (Stale())
			Refresh();
	} else {
		ClearChannels();
		fetchState_ = State::Loading;
	}
	Changed();
}

MultistreamLink::State MultistreamLink::CurrentState() const
{
	if (!stream_.SendToMultistream())
		return State::Off;
	if (!auth_.HasPluginToken())
		return State::SignedOut;
	return fetchState_;
}

std::vector<std::string> MultistreamLink::ChannelNames() const
{
	std::vector<std::string> names;
	for (const relay::Destination &channel : channels_)
		names.push_back(channel.name);
	return names;
}

std::vector<relay::Destination> MultistreamLink::ForStream(const relay::Destination &primary)
{
	outcome_ = StreamOutcome::None;
	outcomeCount_ = 0;
	outcomeReason_ = State::Off;
	if (!stream_.SendToMultistream())
		return {};
	const State state = CurrentState();
	const bool usable = auth_.HasPluginToken() && haveChannels_;
	// OBS can't wait for the network here: this stream uses what is in memory,
	// and a copy older than a minute is fetched again for the next one.
	if (Stale())
		Refresh();
	mslink::ExtraDestinations plan =
		mslink::PlanExtraDestinations(primary, usable ? channels_ : std::vector<relay::Destination>());
	if (plan.cloudIngest) {
		outcome_ = StreamOutcome::CloudIngest;
		TRIGGLOW_LOG_INFO(
			kComponent,
			"this stream goes to Trigglow Multistream, which forwards to the channels: none added");
	} else if (!usable) {
		outcome_ = StreamOutcome::NotReady;
		outcomeReason_ = state;
		TRIGGLOW_LOG_INFO(kComponent,
				  "no Multistream channels in memory: this stream goes to OBS's service only");
	} else {
		outcome_ = StreamOutcome::Added;
		outcomeCount_ = plan.extra.size();
		// The count only: never a key, a server or a name.
		TRIGGLOW_LOG_INFO(kComponent, "%zu Multistream channel(s) added to this stream", plan.extra.size());
	}
	// No Changed(): the controller notifies the dock once the stream is set up.
	return std::move(plan.extra);
}

void MultistreamLink::OnAuthChanged()
{
	const bool signedIn = auth_.HasPluginToken();
	if (signedIn == wasSignedIn_)
		return;
	wasSignedIn_ = signedIn;
	if (signedIn) {
		Refresh();
	} else {
		// Signed out: nothing of the account stays in memory, and answers
		// still on their way are dropped.
		++epoch_;
		ClearChannels();
		fetchState_ = State::Loading;
		fetching_ = false;
		posting_ = false;
		failures_ = 0;
		everPosted_ = false;
		lastSignature_.clear();
	}
	Changed();
}

bool MultistreamLink::Stale() const
{
	return !haveChannels_ || Clock::now() - fetchedAt_ > std::chrono::seconds(mslink::kCacheFreshSeconds);
}

void MultistreamLink::Refresh()
{
	if (fetching_ || !stream_.SendToMultistream() || !auth_.HasPluginToken() || !http::Available())
		return;
	fetching_ = true;
	lastFetchAttempt_ = Clock::now();
	if (!haveChannels_)
		fetchState_ = State::Loading;
	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	// A renewal never runs under this job's cancellation (auth::Renewer).
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const int epoch = epoch_;
	const bool started = uitasks::RunAsync<mslink::FetchResult>(
		tasks_, kComponent,
		[client, store, renewer](const tasks::CancelToken &cancel) {
			return mslink::FetchDestinations(client.WithCancel(&cancel), *store, renewer.get());
		},
		[this, epoch](const mslink::FetchResult &result) { OnFetched(result, epoch); });
	if (!started) {
		fetching_ = false;
		if (!haveChannels_)
			fetchState_ = State::Failed;
	}
}

void MultistreamLink::OnFetched(const mslink::FetchResult &result, int epoch)
{
	auth_.ApplyAuthEvents(result.events);
	if (epoch != epoch_)
		return; // Signed out meanwhile: not this account's answer any more.
	fetching_ = false;
	if (!stream_.SendToMultistream() || !auth_.HasPluginToken())
		return;
	switch (result.status) {
	case mslink::FetchStatus::Ok:
		Wipe(channels_);
		channels_ = result.destinations;
		haveChannels_ = true;
		fetchedAt_ = Clock::now();
		fetchState_ = State::Ready;
		TRIGGLOW_LOG_INFO(kComponent, "%zu Multistream channel(s) ready", channels_.size());
		break;
	case mslink::FetchStatus::PlanRequired:
		ClearChannels();
		fetchState_ = State::PlanRequired;
		TRIGGLOW_LOG_INFO(kComponent, "the account's plan doesn't include sending to Multistream channels");
		break;
	case mslink::FetchStatus::Unavailable:
		ClearChannels();
		fetchState_ = State::Unavailable;
		TRIGGLOW_LOG_INFO(kComponent, "Multistream isn't available for this account");
		break;
	case mslink::FetchStatus::SignedOut:
		ClearChannels();
		fetchState_ = State::Loading;
		break;
	case mslink::FetchStatus::RateLimited:
	case mslink::FetchStatus::Failed:
		// An earlier copy stays usable; without one the dock says why.
		if (!haveChannels_)
			fetchState_ = result.status == mslink::FetchStatus::RateLimited ? State::RateLimited
											: State::Failed;
		TRIGGLOW_LOG_DEBUG(kComponent, "couldn't get the Multistream channels; trying again later");
		break;
	}
	Changed();
}

void MultistreamLink::ClearChannels()
{
	Wipe(channels_);
	haveChannels_ = false;
}

void MultistreamLink::Tick()
{
	const Clock::time_point now = Clock::now();
	if ((fetchState_ == State::Failed || fetchState_ == State::RateLimited) &&
	    now - lastFetchAttempt_ >= kFetchRetry)
		Refresh();

	if (!auth_.HasPluginToken() || !http::Available())
		return;

	mslink::StatusReport report;
	bool busy = false;
	report.streaming = obs_frontend_streaming_active();
	report.streamMode = router_.StreamMode();
	if (report.streamMode) {
		const StreamDelayStatus status = stream_.GetStatus();
		report.active = status.delayOn;
		report.seconds = status.delaySeconds;
		busy = status.state == StreamDelayState::Draining;
		if (status.engaged || status.state == StreamDelayState::Draining) {
			for (const relay::SinkStatus &sink : status.relay.sinks)
				report.channels.push_back({sink.name, sink.state});
		}
	} else {
		const RouterStatus status = router_.Status();
		report.active = status.delayOn; // Filling included.
		report.seconds = status.delaySeconds;
	}
	busy = busy || report.streaming || report.active;

	std::string signature = json::Serialize(mslink::StatusBody(report, {}));
	if (signature != lastSignature_) {
		lastSignature_ = std::move(signature);
		changePending_ = true;
		changeDueAt_ = now + kChangeDebounce;
	}
	if (posting_)
		return;
	const bool backedOff = failures_ >= mslink::kFailuresBeforeBackoff;
	const bool intervalDue = !everPosted_ ||
				 now - lastPost_ >=
					 std::chrono::seconds(mslink::HeartbeatIntervalSeconds(busy, failures_));
	const bool changeDue = changePending_ && !backedOff && now >= changeDueAt_;
	if (!intervalDue && !changeDue)
		return;

	const api::Client client = auth_.ApiClient();
	std::shared_ptr<TokenStore> store = auth_.Store();
	std::shared_ptr<const auth::Renewer> renewer = auth_.TokenRenewer();
	const int epoch = epoch_;
	posting_ = uitasks::RunAsync<mslink::PostResult>(
		tasks_, kComponent,
		[client, store, renewer, report](const tasks::CancelToken &cancel) {
			return mslink::PostStatus(client.WithCancel(&cancel), *store, report, renewer.get());
		},
		[this, epoch](const mslink::PostResult &result) { OnPosted(result, epoch); });
	changePending_ = false;
	everPosted_ = true;
	lastPost_ = now;
}

void MultistreamLink::OnPosted(const mslink::PostResult &result, int epoch)
{
	auth_.ApplyAuthEvents(result.events);
	if (epoch != epoch_)
		return;
	posting_ = false;
	if (!result.ok) {
		// Silent: the dashboard just shows the plugin as not connected.
		if (++failures_ == mslink::kFailuresBeforeBackoff)
			TRIGGLOW_LOG_DEBUG(kComponent, "the status report keeps failing; once a minute until it works");
		return;
	}
	failures_ = 0;
	Apply(result.command);
}

void MultistreamLink::Apply(const mslink::Command &command)
{
	// Through the router, like a hotkey or obs-websocket: to the engine the
	// dock selects, and Enable keeps its free-account gate.
	switch (command.type) {
	case mslink::CommandType::None:
		return;
	case mslink::CommandType::Enable:
		TRIGGLOW_LOG_INFO(kComponent, "from the dashboard: delay on");
		router_.Enable();
		break;
	case mslink::CommandType::Disable:
		TRIGGLOW_LOG_INFO(kComponent, "from the dashboard: delay off");
		router_.Disable();
		break;
	case mslink::CommandType::Dump:
		TRIGGLOW_LOG_INFO(kComponent, "from the dashboard: dump");
		router_.Dump();
		break;
	case mslink::CommandType::SetDelay:
		TRIGGLOW_LOG_INFO(kComponent, "from the dashboard: delay %us", command.seconds);
		router_.SetDelay(command.seconds);
		break;
	}
	Changed();
}

void MultistreamLink::Changed()
{
	if (onChanged_)
		onChanged_();
}

} // namespace trigglow
