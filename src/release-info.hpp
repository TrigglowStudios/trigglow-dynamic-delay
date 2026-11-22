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
#include <optional>
#include <string>
#include <string_view>

// v1.1.0: what the update check reads from GitHub's "latest release" answer,
// and the checks on every URL and hash in it. No libobs, Qt or network here,
// so all of it is unit-tested on its own (tests/release_info_tests.cpp).
//
// The answer comes from the network, so nothing in it is trusted as is:
//   - the version must be plain X.Y.Z (a leading 'v' is dropped);
//   - the Windows installer must be the asset named exactly
//     obs-trigglow-dynamic-delay-<version>-windows-x64-setup.exe, its
//     download URL this repo's release download for that tag and name, and
//     its digest a SHA-256 (GitHub publishes one for every asset);
//   - the release page must be one of this repo's release pages.
// An answer that fails any of these still reports the version (the notice
// shows), just without an installer: the dock then offers the download page
// on trigglow.com instead of updating in place.
namespace trigglow::release {

inline constexpr const char *kRepoPath = "/TrigglowStudios/trigglow-dynamic-delay";
inline constexpr const char *kApiHost = "api.github.com";
inline constexpr const char *kLatestReleaseApiPath = "/repos/TrigglowStudios/trigglow-dynamic-delay/releases/latest";
inline constexpr const char *kReleasePagePrefix = "https://github.com/TrigglowStudios/trigglow-dynamic-delay/releases/";
inline constexpr const char *kDownloadHost = "github.com";

// GitHub's release download answers with a redirect to one of these hosts
// (signed, short-lived URLs). The download follows that one redirect, to
// these hosts only.
inline constexpr const char *kAssetHosts[] = {"release-assets.githubusercontent.com", "objects.githubusercontent.com"};

// The installer is ~40 MB; anything announced above this is refused before
// a byte is downloaded.
inline constexpr uint64_t kMaxInstallerBytes = 512ull * 1024 * 1024;

struct Installer {
	std::string name;   // obs-trigglow-dynamic-delay-1.1.0-windows-x64-setup.exe
	std::string path;   // Path on kDownloadHost: /TrigglowStudios/.../releases/download/1.1.0/<name>
	std::string sha256; // 64 lowercase hex digits.
	uint64_t size = 0;  // Bytes, as GitHub announces them (> 0).
};

struct LatestRelease {
	std::string version; // "1.1.0", no leading 'v'.
	std::string tag;     // The tag as published ("1.1.0" or "v1.1.0").
	std::string pageUrl; // One of this repo's release pages (IsOwnReleasePageUrl), or empty.
	std::optional<Installer> windowsInstaller;
};

// nullopt when the text isn't a release object with a plain X.Y.Z tag.
std::optional<LatestRelease> ParseLatestRelease(std::string_view json);

// True for "X.Y.Z" with 1-4 digit numbers, nothing else.
bool IsPlainVersion(std::string_view version);
// "obs-trigglow-dynamic-delay-<version>-windows-x64-setup.exe".
std::string WindowsInstallerName(std::string_view version);
// True for 64 hex digits (either case).
bool IsSha256Hex(std::string_view hex);
// kReleasePagePrefix followed by plain path segments ([A-Za-z0-9._-]), no
// "..", no query, fragment, escapes, whitespace or control characters.
bool IsOwnReleasePageUrl(std::string_view url);

struct HttpsUrl {
	std::string host;
	std::string path; // Path and query, starting with '/'.
};
// "https://<host>/<rest>" with a plain host (letters, digits, '.', '-'; no
// user info, no port) and a path of printable ASCII without spaces, '#' or
// '\\'. nullopt otherwise.
std::optional<HttpsUrl> SplitHttpsUrl(std::string_view url);
// The redirect a release download may follow: an https URL on one of
// kAssetHosts.
std::optional<HttpsUrl> AllowedAssetRedirect(std::string_view location);

} // namespace trigglow::release
