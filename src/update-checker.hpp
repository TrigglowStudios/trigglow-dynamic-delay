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

#include <optional>
#include <string>

#include "cancel-token.hpp"
#include "release-info.hpp"

// Checks this repo's latest published GitHub release against the running
// plugin's own version: a public, unauthenticated GET against GitHub's REST
// API, no trigglow.com endpoint needed. Kept Qt-free; TrigglowDelayDock runs
// it as one of its background jobs (ui-tasks.hpp), cancelled when the dock or
// OBS closes.
//
// v1.1.0: the answer also carries the Windows installer of that release (its
// download path, size and SHA-256, release-info.hpp), which update-installer.hpp
// downloads and runs so the update happens from OBS itself, without sending
// the streamer to GitHub.
namespace trigglow {

struct UpdateCheckResult {
	// False for ANY failure (no network, GitHub down, unexpected response
	// shape) -- callers must treat this as "unknown," never surface it as an
	// error to the user. This is a nice-to-have notice, not something worth
	// bothering a streamer about failing.
	bool checked = false;
	bool updateAvailable = false;
	std::string latestVersion; // e.g. "1.1.0" (leading 'v' already stripped). Empty if !checked.
	// The installer to update in place (Windows), when the release has a valid one.
	std::optional<release::Installer> windowsInstaller;
};

// BLOCKING -- callers must not run this on the UI thread, same contract as
// every call in win-http.hpp (this is built directly on top of http::Send).
// currentVersion: PLUGIN_VERSION (plugin-support.h), passed in rather than
// read here so this file stays free of that header's C-linkage extern.
// cancel: the job's token; when it is cancelled the request ends at once and
// the result is "not checked", without a log line.
UpdateCheckResult CheckForUpdate(const std::string &currentVersion, const tasks::CancelToken *cancel = nullptr);

} // namespace trigglow
