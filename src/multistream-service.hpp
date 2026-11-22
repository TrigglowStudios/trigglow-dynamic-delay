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

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <QObject>

#include "background-tasks.hpp"
#include "multistream-link.hpp"

class QTimer;

// v1.1.0, the plugin's side of the link to Trigglow Multistream (the protocol
// is multistream-link.hpp). UI thread only; every network call is a background
// job of this object's group, aborted on unload like all the others.
//
//   - "Also send to my Trigglow Multistream channels" (stream mode's option,
//     saved by StreamDelayController): keeps the streamer's channels in
//     memory, fetched when the box is ticked, at load, after sign-in and
//     again when a stream starts with a copy older than a minute, and hands
//     them to the controller as extra destinations of the relay when OBS
//     starts streaming (ForStream()). Their keys are never written anywhere
//     and never logged; signing out or unticking the box wipes them.
//   - While signed in, reports the delay's status for the dashboard (every
//     5 s while OBS streams or the delay is on, every 30 s otherwise, about
//     1 s after a change, once a minute after repeated failures) and applies
//     the command the dashboard may answer with through DelayRouter, like a
//     hotkey: the free-account gate of Enable applies to it too.
namespace trigglow {

class AuthManager;
class DelayRouter;
class StreamDelayController;

class MultistreamLink : public QObject {
public:
	// What the option's hint in the dock says.
	enum class State {
		Off,          // The box isn't ticked.
		SignedOut,    // Ticked, but no Trigglow sign-in.
		Loading,      // Fetching the channels for the first time.
		Ready,        // Channels in memory (maybe none).
		PlanRequired, // The account's plan doesn't include it.
		Unavailable,  // Multistream is off for this account.
		RateLimited,  // Asked too often; tries again.
		Failed,       // Couldn't fetch; tries again.
	};
	// What happened to the option when the current stream started.
	enum class StreamOutcome {
		None,        // The box wasn't ticked then (or no stream yet).
		Added,       // StreamChannelCount() channels were added (maybe 0).
		CloudIngest, // The stream goes to Trigglow's ingest: none added.
		NotReady,    // No channels in memory then: StreamReason() says why.
	};

	MultistreamLink(AuthManager &auth, StreamDelayController &stream, DelayRouter &router);
	~MultistreamLink() override;

	// After the stream settings were loaded: fetches when the option is on and
	// starts the status reports.
	void Init();
	// obs_module_unload(), before the controllers and AuthManager go away.
	void Shutdown();

	// The dock's check box. The controller keeps (and plugin-main.cpp saves)
	// the setting.
	void SetEnabled(bool enabled);

	State CurrentState() const;
	// The channels in memory, names only.
	std::vector<std::string> ChannelNames() const;
	StreamOutcome Outcome() const { return outcome_; }
	size_t StreamChannelCount() const { return outcomeCount_; }
	State StreamReason() const { return outcomeReason_; }

	// StreamDelayController's provider: the destinations to add after
	// `primary` for the stream that is starting, from what is in memory now.
	std::vector<relay::Destination> ForStream(const relay::Destination &primary);

	// Anything the dock shows of this changed.
	void SetChangedCallback(std::function<void()> callback) { onChanged_ = std::move(callback); }

private:
	using Clock = std::chrono::steady_clock;

	void OnAuthChanged();
	void Refresh();
	void OnFetched(const mslink::FetchResult &result, int epoch);
	void ClearChannels();
	bool Stale() const;
	void Tick();
	void OnPosted(const mslink::PostResult &result, int epoch);
	void Apply(const mslink::Command &command);
	void Changed();

	AuthManager &auth_;
	StreamDelayController &stream_;
	DelayRouter &router_;
	QTimer *timer_ = nullptr;
	std::function<void()> onChanged_;
	bool wasSignedIn_ = false;
	// Bumped on sign-out: answers to an earlier sign-in are dropped.
	int epoch_ = 0;

	// The channels, keys included: memory only.
	std::vector<relay::Destination> channels_;
	bool haveChannels_ = false;
	Clock::time_point fetchedAt_{};
	Clock::time_point lastFetchAttempt_{};
	bool fetching_ = false;
	State fetchState_ = State::Loading;
	StreamOutcome outcome_ = StreamOutcome::None;
	size_t outcomeCount_ = 0;
	State outcomeReason_ = State::Off;

	// The status report.
	std::string lastSignature_;
	bool changePending_ = false;
	Clock::time_point changeDueAt_{};
	Clock::time_point lastPost_{};
	bool everPosted_ = false;
	bool posting_ = false;
	int failures_ = 0;

	// Last member, so it is destroyed first: the calls in flight are aborted
	// before anything they report to goes away.
	tasks::Group tasks_;
};

} // namespace trigglow
