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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "cancel-token.hpp"
#include "release-info.hpp"

// v1.1.0: updating the plugin from OBS itself (Windows), instead of sending
// the streamer to GitHub. Qt-free; UpdateDialog runs each step as a
// background job.
//
//   1. DownloadInstaller(): GET the release download on github.com (no
//      redirects followed by the HTTP client), take its one redirect only to
//      GitHub's asset hosts (release-info.hpp), stream the body to
//      <configDir>/updates/<name>.part while hashing it, and keep it only if
//      the size and the SHA-256 are exactly the ones GitHub's API announced.
//   2. LaunchInstaller(): open the file without sharing write or delete (so
//      it can't change between the check and the start), hash it again, and
//      start it with ShellExecuteEx as the signed-in user:
//        /SILENT /LANG=<spanish|english> /TRIGGLOWUPDATE=<event>
//      Inno Setup asks Windows for admin rights (UAC) itself. Once it has them
//      it signals the named event (installers/windows/...-setup.iss), then
//      waits for OBS to close, installs with only its progress window, and
//      starts OBS again as the signed-in user. The plugin closes OBS only
//      after that signal: if the streamer says no to UAC, Setup ends without
//      signalling and OBS stays open.
//
// Only offered when CanInstallInPlace(): the plugin was loaded from the
// folder the installer writes to (an installer update elsewhere would leave
// two copies).
namespace trigglow::updater {

// Windows, and this module loaded from
// %ProgramData%\obs-studio\plugins\obs-trigglow-dynamic-delay\ (or its
// bin\64bit). Always false elsewhere.
bool CanInstallInPlace();

enum class DownloadError : unsigned char {
	None,
	Unavailable, // No HTTP client / not Windows.
	Network,     // DNS, connection, TLS, timeout, HTTP error.
	Redirect,    // GitHub answered without a redirect to its asset hosts.
	Size,        // Not the announced size.
	Hash,        // Not the announced SHA-256.
	Disk,        // Couldn't write the file.
	Cancelled,
};

struct DownloadResult {
	DownloadError error = DownloadError::Network;
	std::filesystem::path file; // The verified installer, when error == None.
};

// BLOCKING. progress(received, total) is called on the calling thread as
// bytes arrive (total = installer.size). Old files in `updatesDir` are
// deleted first.
DownloadResult DownloadInstaller(const release::Installer &installer, const std::filesystem::path &updatesDir,
				 const std::string &userAgent,
				 const std::function<void(uint64_t received, uint64_t total)> &progress,
				 const tasks::CancelToken *cancel);

enum class LaunchOutcome : unsigned char {
	Ready,       // Setup has admin rights and waits for OBS to close.
	Declined,    // Setup ended before that: UAC refused, or Setup cancelled.
	Failed,      // Couldn't start it, or the file changed since it was checked.
	TimedOut,    // No answer from Setup in time (it keeps running).
	Cancelled,   // `cancel` was cancelled (OBS is closing).
	Unavailable, // Not Windows.
};

struct LaunchResult {
	LaunchOutcome outcome = LaunchOutcome::Failed;
	unsigned long code = 0; // Setup's exit code (Declined) or the Windows error (Failed).
};

// BLOCKING until Setup signals it is ready, exits, `timeoutMs` passes or
// `cancel` is cancelled. spanish: Setup's language (it has Spanish and
// English only).
LaunchResult LaunchInstaller(const std::filesystem::path &file, const std::string &sha256, bool spanish,
			     unsigned long timeoutMs, const tasks::CancelToken *cancel);

// The version whose "update available" window was shown at OBS start
// (<configDir>/update.json), so it shows once per version. Empty if none.
std::string LoadPromptedVersion(const std::filesystem::path &configDir);
void SavePromptedVersion(const std::filesystem::path &configDir, const std::string &version);

} // namespace trigglow::updater
