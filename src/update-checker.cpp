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

#include "update-checker.hpp"
#include "api-client.hpp"
#include "logging.hpp"
#include "win-http.hpp"

#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#endif

namespace trigglow {

namespace {
constexpr const char *kComponent = "update-checker";
} // namespace

UpdateCheckResult CheckForUpdate(const std::string &currentVersion, const tasks::CancelToken *cancel)
{
	UpdateCheckResult result;

	// GitHub's REST API requires a User-Agent header on every request (403
	// otherwise): the plugin's own. No token, no cookies, redirects not
	// followed (win-http.hpp).
	http::Request request;
	request.host = release::kApiHost;
	request.path = release::kLatestReleaseApiPath;
	request.userAgent = api::UserAgent(currentVersion);
	request.headers.push_back({"Accept", "application/vnd.github+json"});
	request.headers.push_back({"X-GitHub-Api-Version", "2022-11-28"});
	request.maxResponseBytes = 1024 * 1024; // The release notes travel along.
	request.cancel = cancel;
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	// Local test builds only: TRIGGLOW_TEST_UPDATE_DIR=<folder> answers with
	// <folder>/latest.json (GitHub's format) instead of asking GitHub
	// (update-installer.cpp reads the installer from there too).
	http::Response response;
	if (const char *dir = std::getenv("TRIGGLOW_TEST_UPDATE_DIR"); dir && *dir) {
		TRIGGLOW_LOG_WARN(kComponent,
				  "TEST BUILD: update check reads latest.json from TRIGGLOW_TEST_UPDATE_DIR");
		std::ifstream file(std::filesystem::u8path(dir) / "latest.json", std::ios::binary);
		response.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		response.error = file ? http::Error::None : http::Error::Connect;
		response.status = 200;
	} else {
		response = http::Send(request);
	}
#else
	const http::Response response = http::Send(request);
#endif
	if (response.error == http::Error::Cancelled)
		return result; // The dock or OBS is closing.
	if (!response.Ok() || response.status != 200) {
		TRIGGLOW_LOG_INFO(kComponent, "update check failed (HTTP %d, %s) -- treating as unknown, not an error",
				  response.status, http::ErrorName(response.error));
		return result;
	}

	const std::optional<release::LatestRelease> latest = release::ParseLatestRelease(response.body);
	if (!latest) {
		TRIGGLOW_LOG_WARN(kComponent, "update check: unexpected answer from GitHub (no plain X.Y.Z release)");
		return result;
	}

	result.checked = true;
	result.latestVersion = latest->version;
	result.updateAvailable = api::CompareVersions(latest->version, api::ClientVersion(currentVersion)) > 0;
	result.windowsInstaller = latest->windowsInstaller;

	TRIGGLOW_LOG_INFO(
		kComponent, "update check: running v%s, latest published is v%s (%s%s)", currentVersion.c_str(),
		latest->version.c_str(), result.updateAvailable ? "update available" : "up to date",
		result.updateAvailable && !result.windowsInstaller ? ", no verifiable Windows installer" : "");

	return result;
}

} // namespace trigglow
