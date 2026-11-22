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

// v1.1.0: release-info.hpp -- what the update check takes from GitHub's
// "latest release" answer, and the URL and hash checks the in-OBS update
// relies on before it downloads and starts anything.

#include "release-info.hpp"
#include "test-support.hpp"

#include <string>

namespace release = trigglow::release;

namespace {

const std::string kSha = "230dc30c600daabe67e22b8e3b36551a76e2ca23583da0956db5e228fc33b6ab";

std::string DownloadUrl(const std::string &tag, const std::string &name)
{
	return std::string("https://") + release::kDownloadHost + release::kRepoPath + "/releases/download/" + tag +
	       "/" + name;
}

// A release answer like GitHub's, with one installer asset whose fields can
// be bent per test.
std::string Answer(const std::string &tag, const std::string &assetName, const std::string &url,
		   const std::string &digest, const std::string &size = "41064232", const std::string &extra = "")
{
	return std::string("{\"tag_name\":\"") + tag + "\",\"draft\":false,\"prerelease\":false," + "\"html_url\":\"" +
	       release::kReleasePagePrefix + "tag/" + tag + "\"," + extra +
	       "\"assets\":[{\"name\":\"notes.txt\",\"browser_download_url\":\"x\",\"size\":1}," + "{\"name\":\"" +
	       assetName + "\",\"state\":\"uploaded\",\"size\":" + size + "," + "\"digest\":\"" + digest +
	       "\",\"browser_download_url\":\"" + url + "\"}]}";
}

std::string GoodAnswer(const std::string &tag = "1.1.0")
{
	const std::string version = tag[0] == 'v' ? tag.substr(1) : tag;
	const std::string name = release::WindowsInstallerName(version);
	return Answer(tag, name, DownloadUrl(tag, name), "sha256:" + kSha);
}

void TestVersions()
{
	testcheck::g_currentTest = "versions";
	CHECK(release::IsPlainVersion("1.1.0"));
	CHECK(release::IsPlainVersion("10.20.300"));
	CHECK(!release::IsPlainVersion("1.1"));
	CHECK(!release::IsPlainVersion("1.1.0.0"));
	CHECK(!release::IsPlainVersion("1.1.0-rc1"));
	CHECK(!release::IsPlainVersion("v1.1.0"));
	CHECK(!release::IsPlainVersion("1..0"));
	CHECK(!release::IsPlainVersion("1.1.12345"));
	CHECK(!release::IsPlainVersion("1.1.0/../x"));
	CHECK(!release::IsPlainVersion(""));
	CHECK_STR(release::WindowsInstallerName("1.1.0"), "obs-trigglow-dynamic-delay-1.1.0-windows-x64-setup.exe");
}

void TestParse()
{
	testcheck::g_currentTest = "parse";
	auto r = release::ParseLatestRelease(GoodAnswer());
	CHECK(r.has_value());
	if (r) {
		CHECK_STR(r->version, "1.1.0");
		CHECK(r->windowsInstaller.has_value());
		CHECK_STR(r->pageUrl, std::string(release::kReleasePagePrefix) + "tag/1.1.0");
		if (r->windowsInstaller) {
			CHECK_STR(r->windowsInstaller->sha256, kSha);
			CHECK_EQ(r->windowsInstaller->size, 41064232ull);
			CHECK_STR(
				r->windowsInstaller->path,
				std::string(release::kRepoPath) +
					"/releases/download/1.1.0/obs-trigglow-dynamic-delay-1.1.0-windows-x64-setup.exe");
		}
	}

	// A "v" tag: version without it, download path with it.
	r = release::ParseLatestRelease(GoodAnswer("v1.2.0"));
	CHECK(r && r->version == "1.2.0" && r->windowsInstaller &&
	      r->windowsInstaller->path.find("/download/v1.2.0/") != std::string::npos);

	// Uppercase digests are accepted and kept lowercase.
	std::string upper = kSha;
	for (char &c : upper)
		c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
	const std::string name = release::WindowsInstallerName("1.1.0");
	r = release::ParseLatestRelease(Answer("1.1.0", name, DownloadUrl("1.1.0", name), "sha256:" + upper));
	CHECK(r && r->windowsInstaller && r->windowsInstaller->sha256 == kSha);
}

void TestRejectedReleases()
{
	testcheck::g_currentTest = "rejected releases";
	CHECK(!release::ParseLatestRelease("").has_value());
	CHECK(!release::ParseLatestRelease("[]").has_value());
	CHECK(!release::ParseLatestRelease("{\"message\":\"Not Found\"}").has_value());
	CHECK(!release::ParseLatestRelease("{\"tag_name\":\"1.1.0-beta\"}").has_value());
	CHECK(!release::ParseLatestRelease("{\"tag_name\":\"../1.1.0\"}").has_value());
	CHECK(!release::ParseLatestRelease("{\"tag_name\":\"1.1.0\",\"draft\":true}").has_value());
	CHECK(!release::ParseLatestRelease("{\"tag_name\":\"1.1.0\",\"prerelease\":true}").has_value());
	// No assets: the version still counts, without an installer.
	auto r = release::ParseLatestRelease("{\"tag_name\":\"1.1.0\"}");
	CHECK(r && r->version == "1.1.0" && !r->windowsInstaller && r->pageUrl.empty());
}

void TestRejectedInstallers()
{
	testcheck::g_currentTest = "rejected installers";
	const std::string name = release::WindowsInstallerName("1.1.0");
	const std::string url = DownloadUrl("1.1.0", name);
	auto noInstaller = [](const std::string &answer) {
		const auto r = release::ParseLatestRelease(answer);
		return r.has_value() && !r->windowsInstaller.has_value();
	};
	// Another file name (an older version's, or another asset).
	const std::string old = release::WindowsInstallerName("1.0.0");
	CHECK(noInstaller(Answer("1.1.0", old, DownloadUrl("1.1.0", old), "sha256:" + kSha)));
	// The URL points elsewhere: another host, repo, tag, or with a query.
	CHECK(noInstaller(Answer("1.1.0", name, "https://example.invalid/" + name, "sha256:" + kSha)));
	CHECK(noInstaller(Answer("1.1.0", name,
				 std::string("https://") + release::kDownloadHost +
					 "/someone/else/releases/download/1.1.0/" + name,
				 "sha256:" + kSha)));
	CHECK(noInstaller(Answer("1.1.0", name, DownloadUrl("1.0.0", name), "sha256:" + kSha)));
	CHECK(noInstaller(Answer("1.1.0", name, url + "?x=1", "sha256:" + kSha)));
	CHECK(noInstaller(Answer("1.1.0", name, "http" + url.substr(5), "sha256:" + kSha)));
	// Digest missing, of another kind, or not 64 hex digits.
	CHECK(noInstaller(Answer("1.1.0", name, url, "")));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha1:" + kSha.substr(0, 40))));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha.substr(1))));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha.substr(1) + "g")));
	// Size zero, negative, fractional or absurd.
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha, "0")));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha, "-5")));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha, "10.5")));
	CHECK(noInstaller(Answer("1.1.0", name, url, "sha256:" + kSha, "99999999999")));
	// An upload that isn't finished.
	std::string answer = Answer("1.1.0", name, url, "sha256:" + kSha);
	const size_t at = answer.find("\"state\":\"uploaded\"");
	answer.replace(at, std::string("\"state\":\"uploaded\"").size(), "\"state\":\"starter\"");
	CHECK(noInstaller(answer));
}

void TestReleasePage()
{
	testcheck::g_currentTest = "release page";
	const std::string prefix = release::kReleasePagePrefix;
	CHECK(release::IsOwnReleasePageUrl(prefix + "tag/1.1.0"));
	CHECK(release::IsOwnReleasePageUrl(prefix + "latest"));
	CHECK(!release::IsOwnReleasePageUrl(prefix));
	CHECK(!release::IsOwnReleasePageUrl(prefix + "tag/../../x"));
	CHECK(!release::IsOwnReleasePageUrl(prefix + "tag//1.1.0"));
	CHECK(!release::IsOwnReleasePageUrl(prefix + "tag/1.1.0?x"));
	CHECK(!release::IsOwnReleasePageUrl(prefix + "tag/1.1.0#x"));
	CHECK(!release::IsOwnReleasePageUrl(prefix + "tag/%2e%2e"));
	CHECK(!release::IsOwnReleasePageUrl("https://example.invalid/releases/tag/1.1.0"));
}

void TestRedirects()
{
	testcheck::g_currentTest = "redirects";
	const std::string assets = std::string("https://") + release::kAssetHosts[0];
	auto ok = release::AllowedAssetRedirect(assets +
						"/github-production-release-asset/1/2?sp=r&sv=2018-11-09&sig=a%2Bb");
	CHECK(ok.has_value());
	if (ok) {
		CHECK_STR(ok->host, release::kAssetHosts[0]);
		CHECK_STR(ok->path, "/github-production-release-asset/1/2?sp=r&sv=2018-11-09&sig=a%2Bb");
	}
	CHECK(release::AllowedAssetRedirect(std::string("https://") + release::kAssetHosts[1] + "/x").has_value());
	// The host is compared whole, after lowercasing.
	CHECK(release::AllowedAssetRedirect("https://RELEASE-ASSETS.githubusercontent.com/x").has_value());
	// Not https, another host, a look-alike, user info, a port, no path.
	CHECK(!release::AllowedAssetRedirect(std::string("http://") + release::kAssetHosts[0] + "/x"));
	CHECK(!release::AllowedAssetRedirect("https://example.invalid/x"));
	CHECK(!release::AllowedAssetRedirect(assets + ".example.invalid/x"));
	CHECK(!release::AllowedAssetRedirect("https://evil.invalid@" + std::string(release::kAssetHosts[0]) + "/x"));
	CHECK(!release::AllowedAssetRedirect(assets + ":8443/x"));
	CHECK(!release::AllowedAssetRedirect(assets));
	// Spaces, control characters, fragments and backslashes in the path.
	CHECK(!release::AllowedAssetRedirect(assets + "/a b"));
	CHECK(!release::AllowedAssetRedirect(assets + "/a\r\nX-Injected: 1"));
	CHECK(!release::AllowedAssetRedirect(assets + "/a#b"));
	CHECK(!release::AllowedAssetRedirect(assets + "/a\\b"));
	CHECK(!release::AllowedAssetRedirect(""));
}

void TestSha()
{
	testcheck::g_currentTest = "sha256";
	CHECK(release::IsSha256Hex(kSha));
	CHECK(!release::IsSha256Hex(kSha.substr(2)));
	CHECK(!release::IsSha256Hex(kSha + "00"));
	CHECK(!release::IsSha256Hex(std::string(64, 'z')));
}

} // namespace

int main()
{
	TestVersions();
	TestParse();
	TestRejectedReleases();
	TestRejectedInstallers();
	TestReleasePage();
	TestRedirects();
	TestSha();
	return testcheck::Finish("release_info_tests");
}
