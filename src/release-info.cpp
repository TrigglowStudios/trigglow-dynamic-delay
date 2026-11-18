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

#include "release-info.hpp"
#include "json.hpp"

#include <cmath>

namespace trigglow::release {

namespace {

bool IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsHexDigit(char c)
{
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsPlainPathChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '.' || c == '-' || c == '_';
}

std::string ToLower(std::string_view s)
{
	std::string out(s);
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return out;
}

std::optional<Installer> FindWindowsInstaller(const json::Value &assets, const std::string &tag,
					      const std::string &version)
{
	if (!assets.IsArray())
		return std::nullopt;
	const std::string name = WindowsInstallerName(version);
	const std::string expectedUrl =
		std::string("https://") + kDownloadHost + kRepoPath + "/releases/download/" + tag + "/" + name;
	for (size_t i = 0; i < assets.Size(); ++i) {
		const json::Value &asset = assets.At(i);
		if (asset.GetString("name").value_or("") != name)
			continue;
		// Only a finished upload ("uploaded"); a missing state reads as one.
		if (asset.GetString("state").value_or("uploaded") != "uploaded")
			return std::nullopt;
		if (asset.GetString("browser_download_url").value_or("") != expectedUrl)
			return std::nullopt;
		const std::string digest = asset.GetString("digest").value_or("");
		constexpr std::string_view kPrefix = "sha256:";
		if (digest.compare(0, kPrefix.size(), kPrefix) != 0)
			return std::nullopt;
		const std::string_view hex = std::string_view(digest).substr(kPrefix.size());
		if (!IsSha256Hex(hex))
			return std::nullopt;
		const double size = asset.GetNumber("size").value_or(0.0);
		if (!(size >= 1.0) || size > static_cast<double>(kMaxInstallerBytes) || std::floor(size) != size)
			return std::nullopt;
		Installer installer;
		installer.name = name;
		installer.path = std::string(kRepoPath) + "/releases/download/" + tag + "/" + name;
		installer.sha256 = ToLower(hex);
		installer.size = static_cast<uint64_t>(size);
		return installer;
	}
	return std::nullopt;
}

} // namespace

bool IsPlainVersion(std::string_view version)
{
	int parts = 0;
	size_t digits = 0;
	for (char c : version) {
		if (IsDigit(c)) {
			if (++digits > 4)
				return false;
		} else if (c == '.') {
			if (digits == 0 || ++parts > 2)
				return false;
			digits = 0;
		} else {
			return false;
		}
	}
	return parts == 2 && digits > 0;
}

std::string WindowsInstallerName(std::string_view version)
{
	return "obs-trigglow-dynamic-delay-" + std::string(version) + "-windows-x64-setup.exe";
}

bool IsSha256Hex(std::string_view hex)
{
	if (hex.size() != 64)
		return false;
	for (char c : hex) {
		if (!IsHexDigit(c))
			return false;
	}
	return true;
}

bool IsOwnReleasePageUrl(std::string_view url)
{
	const std::string_view prefix = kReleasePagePrefix;
	if (url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix)
		return false;
	char previous = '/';
	for (size_t i = prefix.size(); i < url.size(); ++i) {
		const char c = url[i];
		if (!IsPlainPathChar(c) && c != '/')
			return false;
		if (c == '/' && previous == '/')
			return false;
		previous = c;
	}
	return url.find("..", prefix.size()) == std::string_view::npos;
}

std::optional<HttpsUrl> SplitHttpsUrl(std::string_view url)
{
	constexpr std::string_view kScheme = "https://";
	if (url.size() <= kScheme.size() || url.substr(0, kScheme.size()) != kScheme)
		return std::nullopt;
	const std::string_view rest = url.substr(kScheme.size());
	const size_t slash = rest.find('/');
	if (slash == std::string_view::npos || slash == 0 || slash > 253)
		return std::nullopt;
	HttpsUrl out;
	out.host = ToLower(rest.substr(0, slash));
	for (char c : out.host) {
		const bool ok = (c >= 'a' && c <= 'z') || IsDigit(c) || c == '.' || c == '-';
		if (!ok)
			return std::nullopt; // '@' (user info), ':' (port) and anything else.
	}
	if (out.host.front() == '.' || out.host.back() == '.' || out.host.find("..") != std::string::npos)
		return std::nullopt;
	const std::string_view path = rest.substr(slash);
	if (path.size() > 8192)
		return std::nullopt;
	for (char c : path) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (u <= 0x20 || u >= 0x7F || c == '#' || c == '\\')
			return std::nullopt;
	}
	out.path = std::string(path);
	return out;
}

std::optional<HttpsUrl> AllowedAssetRedirect(std::string_view location)
{
	std::optional<HttpsUrl> url = SplitHttpsUrl(location);
	if (!url)
		return std::nullopt;
	for (const char *host : kAssetHosts) {
		if (url->host == host)
			return url;
	}
	return std::nullopt;
}

std::optional<LatestRelease> ParseLatestRelease(std::string_view text)
{
	const std::optional<json::Value> root = json::Parse(text);
	if (!root || !root->IsObject())
		return std::nullopt;
	// "latest" never returns a draft or a pre-release; refuse them anyway.
	if (root->GetBool("draft").value_or(false) || root->GetBool("prerelease").value_or(false))
		return std::nullopt;
	const std::string tag = root->GetString("tag_name").value_or("");
	std::string version = tag;
	if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
		version.erase(0, 1);
	if (!IsPlainVersion(version))
		return std::nullopt;

	LatestRelease release;
	release.version = version;
	release.tag = tag;
	const std::string page = root->GetString("html_url").value_or("");
	release.pageUrl = IsOwnReleasePageUrl(page) ? page : std::string();
	if (const json::Value *assets = root->Find("assets"))
		release.windowsInstaller = FindWindowsInstaller(*assets, tag, version);
	return release;
}

} // namespace trigglow::release
