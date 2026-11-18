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

#include "update-installer.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "win-http.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace trigglow::updater {

namespace fs = std::filesystem;

namespace {

constexpr const char *kComponent = "updater";
constexpr const char *kPrefsFile = "update.json";

#ifdef _WIN32

// SHA-256 over BCrypt (Windows' own CNG), fed in chunks.
class Sha256 {
public:
	Sha256()
	{
		if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
			alg_ = nullptr;
		else if (!BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0)))
			hash_ = nullptr;
	}
	~Sha256()
	{
		if (hash_)
			BCryptDestroyHash(hash_);
		if (alg_)
			BCryptCloseAlgorithmProvider(alg_, 0);
	}
	Sha256(const Sha256 &) = delete;
	Sha256 &operator=(const Sha256 &) = delete;

	bool Valid() const { return hash_ != nullptr; }
	bool Update(const void *data, size_t size)
	{
		return hash_ && BCRYPT_SUCCESS(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void *>(data)),
							      static_cast<ULONG>(size), 0));
	}
	// Lowercase hex, empty on failure.
	std::string Final()
	{
		unsigned char digest[32] = {};
		if (!hash_ || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, sizeof(digest), 0)))
			return std::string();
		static const char kHex[] = "0123456789abcdef";
		std::string out;
		out.reserve(64);
		for (unsigned char b : digest) {
			out += kHex[b >> 4];
			out += kHex[b & 0x0F];
		}
		return out;
	}

private:
	BCRYPT_ALG_HANDLE alg_ = nullptr;
	BCRYPT_HASH_HANDLE hash_ = nullptr;
};

class OwnedHandle {
public:
	explicit OwnedHandle(HANDLE h = nullptr) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
	~OwnedHandle()
	{
		if (h_)
			CloseHandle(h_);
	}
	OwnedHandle(const OwnedHandle &) = delete;
	OwnedHandle &operator=(const OwnedHandle &) = delete;
	HANDLE get() const { return h_; }

private:
	HANDLE h_;
};

std::wstring LowerPath(const fs::path &path)
{
	std::wstring s = path.lexically_normal().wstring();
	while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/'))
		s.pop_back();
	if (!s.empty())
		CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
	return s;
}

bool IsRedirect(int status)
{
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
// Local test builds only (TRIGGLOW_TEST_UPDATE_SOURCE, refused in CI): with
// TRIGGLOW_TEST_UPDATE_DIR=<folder> in OBS's environment, the installer is
// read from <folder>/<name> instead of GitHub, at about 7 MB/s so the
// progress shows; the size and SHA-256 checks, the launch and the installer
// handshake are the real ones (update-checker.cpp reads latest.json there).
fs::path TestUpdateDir()
{
	const char *dir = std::getenv("TRIGGLOW_TEST_UPDATE_DIR");
	return dir && *dir ? fs::u8path(dir) : fs::path();
}

http::Response ReadTestInstaller(const fs::path &dir, const std::string &name, const http::Request &request)
{
	http::Response response;
	std::ifstream in(dir / fs::u8path(name), std::ios::binary);
	if (!in) {
		response.error = http::Error::Connect;
		return response;
	}
	std::vector<char> chunk(256 * 1024);
	size_t total = 0;
	while (in) {
		if (request.cancel && request.cancel->IsCancelled()) {
			response.error = http::Error::Cancelled;
			return response;
		}
		in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		const size_t got = static_cast<size_t>(in.gcount());
		if (got == 0)
			break;
		total += got;
		if (total > request.maxResponseBytes) {
			response.error = http::Error::TooLarge;
			return response;
		}
		if (!request.onBody(chunk.data(), got)) {
			response.error = http::Error::Cancelled;
			return response;
		}
		Sleep(35);
	}
	response.error = http::Error::None;
	response.status = 200;
	return response;
}
#endif

#endif // _WIN32

} // namespace

bool CanInstallInPlace()
{
#ifdef _WIN32
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&CanInstallInPlace), &self))
		return false;
	std::wstring modulePath(32768, L'\0');
	const DWORD length = GetModuleFileNameW(self, modulePath.data(), static_cast<DWORD>(modulePath.size()));
	if (length == 0 || length >= modulePath.size())
		return false;
	modulePath.resize(length);

	PWSTR programData = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)))
		return false;
	const fs::path pluginDir = fs::path(programData) / L"obs-studio" / L"plugins" / L"obs-trigglow-dynamic-delay";
	CoTaskMemFree(programData);

	const std::wstring moduleDir = LowerPath(fs::path(modulePath).parent_path());
	return moduleDir == LowerPath(pluginDir) || moduleDir == LowerPath(pluginDir / L"bin" / L"64bit");
#else
	return false;
#endif
}

DownloadResult DownloadInstaller(const release::Installer &installer, const fs::path &updatesDir,
				 const std::string &userAgent,
				 const std::function<void(uint64_t received, uint64_t total)> &progress,
				 const tasks::CancelToken *cancel)
{
	DownloadResult result;
#ifdef _WIN32
	std::error_code ec;
	fs::create_directories(updatesDir, ec);
	if (ec) {
		result.error = DownloadError::Disk;
		return result;
	}
	// Installers from an earlier update (or a download cut short) go first.
	for (const fs::directory_entry &entry : fs::directory_iterator(updatesDir, ec)) {
		std::error_code removeError;
		if (entry.is_regular_file(removeError) || entry.is_symlink(removeError))
			fs::remove(entry.path(), removeError);
	}

#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	const fs::path testDir = TestUpdateDir();
	std::optional<release::HttpsUrl> target;
	if (testDir.empty()) {
#endif
		// 1. The release download on github.com: a redirect to a signed, short
		// lived URL on GitHub's asset hosts, never followed by the client.
		http::Request first;
		first.host = release::kDownloadHost;
		first.path = installer.path;
		first.userAgent = userAgent;
		first.maxResponseBytes = 64 * 1024;
		first.cancel = cancel;
		const http::Response answer = http::Send(first);
		if (answer.error == http::Error::Cancelled) {
			result.error = DownloadError::Cancelled;
			return result;
		}
		if (!answer.Ok()) {
			TRIGGLOW_LOG_INFO(kComponent, "download: github.com unreachable (%s)",
					  http::ErrorName(answer.error));
			result.error = DownloadError::Network;
			return result;
		}
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
		target =
#else
	const std::optional<release::HttpsUrl> target =
#endif
			IsRedirect(answer.status) ? release::AllowedAssetRedirect(answer.location) : std::nullopt;
		if (!target) {
			TRIGGLOW_LOG_WARN(kComponent,
					  "download: github.com answered HTTP %d without a redirect to its asset hosts",
					  answer.status);
			result.error = answer.status >= 400 ? DownloadError::Network : DownloadError::Redirect;
			return result;
		}
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	}
#endif

	// 2. The file itself, streamed to disk and hashed on the way.
	const fs::path part = updatesDir / fs::u8path(installer.name + ".part");
	const fs::path finalPath = updatesDir / fs::u8path(installer.name);
	std::ofstream out(part, std::ios::binary | std::ios::trunc);
	if (!out) {
		result.error = DownloadError::Disk;
		return result;
	}
	Sha256 hasher;
	if (!hasher.Valid()) {
		result.error = DownloadError::Hash;
		return result;
	}
	uint64_t received = 0;
	bool diskFailed = false;
	http::Request second;
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	if (target) {
#endif
		second.host = target->host;
		second.path = target->path;
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	}
#endif
	second.userAgent = userAgent;
	second.maxResponseBytes = static_cast<size_t>(installer.size);
	second.cancel = cancel;
	second.onBody = [&](const char *data, size_t size) {
		out.write(data, static_cast<std::streamsize>(size));
		if (!out) {
			diskFailed = true;
			return false;
		}
		hasher.Update(data, size);
		received += size;
		if (progress)
			progress(received, installer.size);
		return true;
	};
#ifdef TRIGGLOW_TEST_UPDATE_SOURCE
	const http::Response body = testDir.empty() ? http::Send(second)
						    : ReadTestInstaller(testDir, installer.name, second);
#else
	const http::Response body = http::Send(second);
#endif
	out.close();

	auto fail = [&](DownloadError error) {
		std::error_code removeError;
		fs::remove(part, removeError);
		result.error = error;
		return result;
	};
	if (diskFailed || out.fail())
		return fail(DownloadError::Disk);
	if (body.error == http::Error::Cancelled)
		return fail(DownloadError::Cancelled);
	if (body.error == http::Error::TooLarge)
		return fail(DownloadError::Size);
	if (!body.Ok() || body.status != 200) {
		TRIGGLOW_LOG_INFO(kComponent, "download: asset host answered HTTP %d (%s)", body.status,
				  http::ErrorName(body.error));
		return fail(DownloadError::Network);
	}
	if (received != installer.size) {
		TRIGGLOW_LOG_WARN(kComponent, "download: %llu bytes, %llu announced",
				  static_cast<unsigned long long>(received),
				  static_cast<unsigned long long>(installer.size));
		return fail(DownloadError::Size);
	}
	if (hasher.Final() != installer.sha256) {
		TRIGGLOW_LOG_WARN(kComponent, "download: SHA-256 doesn't match the one GitHub announced, file deleted");
		return fail(DownloadError::Hash);
	}
	fs::remove(finalPath, ec);
	fs::rename(part, finalPath, ec);
	if (ec)
		return fail(DownloadError::Disk);
	TRIGGLOW_LOG_INFO(kComponent, "download: %s verified (%llu bytes, SHA-256 as announced)",
			  installer.name.c_str(), static_cast<unsigned long long>(received));
	result.error = DownloadError::None;
	result.file = finalPath;
	return result;
#else
	(void)installer;
	(void)updatesDir;
	(void)userAgent;
	(void)progress;
	(void)cancel;
	result.error = DownloadError::Unavailable;
	return result;
#endif
}

LaunchResult LaunchInstaller(const fs::path &file, const std::string &sha256, bool spanish, unsigned long timeoutMs,
			     const tasks::CancelToken *cancel)
{
	LaunchResult result;
#ifdef _WIN32
	// Held without write or delete sharing until Setup answers, so the file
	// that runs is the one hashed here. Opened as is if it's a link (then
	// refused below).
	OwnedHandle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
				       nullptr));
	if (!handle.get()) {
		result.code = GetLastError();
		return result;
	}
	BY_HANDLE_FILE_INFORMATION info = {};
	if (!GetFileInformationByHandle(handle.get(), &info) ||
	    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
		TRIGGLOW_LOG_WARN(kComponent, "launch: the installer is not a regular file, not started");
		return result;
	}
	Sha256 hasher;
	std::vector<char> buffer(256 * 1024);
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
			result.code = GetLastError();
			return result;
		}
		if (read == 0)
			break;
		hasher.Update(buffer.data(), read);
	}
	if (hasher.Final() != sha256) {
		TRIGGLOW_LOG_WARN(kComponent, "launch: the installer changed since it was checked, not started");
		return result;
	}

	// The event Setup sets once it has admin rights (the .iss checks the name).
	wchar_t eventName[96] = {};
	swprintf_s(eventName, L"Local\\TrigglowDynamicDelayUpdate-%lu-%llu", GetCurrentProcessId(),
		   static_cast<unsigned long long>(GetTickCount64()));
	OwnedHandle ready(CreateEventW(nullptr, TRUE, FALSE, eventName));
	OwnedHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!ready.get() || !stop.get()) {
		result.code = GetLastError();
		return result;
	}

	const std::wstring parameters = std::wstring(L"/SILENT /NORESTART /LANG=") +
					(spanish ? L"spanish" : L"english") + L" /TRIGGLOWUPDATE=" + eventName;
	const std::wstring directory = file.parent_path().wstring();
	SHELLEXECUTEINFOW sei = {};
	sei.cbSize = sizeof(sei);
	sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	sei.lpVerb = L"open"; // Setup elevates itself, so it can start OBS again as this user.
	sei.lpFile = file.c_str();
	sei.lpParameters = parameters.c_str();
	sei.lpDirectory = directory.c_str();
	sei.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&sei) || !sei.hProcess) {
		result.code = GetLastError();
		TRIGGLOW_LOG_WARN(kComponent, "launch: couldn't start the installer (error %lu)", result.code);
		return result;
	}
	OwnedHandle process(sei.hProcess);
	TRIGGLOW_LOG_INFO(kComponent, "launch: installer started, waiting for it to get admin rights");

	const tasks::CancelSubscription onCancel(cancel, [event = stop.get()] { SetEvent(event); });
	if (cancel && cancel->IsCancelled())
		SetEvent(stop.get());
	const HANDLE waits[3] = {ready.get(), process.get(), stop.get()};
	const DWORD waited = WaitForMultipleObjects(3, waits, FALSE, timeoutMs);
	switch (waited) {
	case WAIT_OBJECT_0:
		result.outcome = LaunchOutcome::Ready;
		TRIGGLOW_LOG_INFO(kComponent, "launch: installer ready, OBS can close");
		break;
	case WAIT_OBJECT_0 + 1: {
		DWORD code = 0;
		GetExitCodeProcess(process.get(), &code);
		result.outcome = LaunchOutcome::Declined;
		result.code = code;
		TRIGGLOW_LOG_INFO(kComponent, "launch: installer ended before it was ready (exit code %lu)",
				  static_cast<unsigned long>(code));
		break;
	}
	case WAIT_OBJECT_0 + 2:
		result.outcome = LaunchOutcome::Cancelled;
		break;
	case WAIT_TIMEOUT:
		result.outcome = LaunchOutcome::TimedOut;
		TRIGGLOW_LOG_WARN(kComponent, "launch: no answer from the installer in time");
		break;
	default:
		result.code = GetLastError();
		break;
	}
	return result;
#else
	(void)file;
	(void)sha256;
	(void)spanish;
	(void)timeoutMs;
	(void)cancel;
	result.outcome = LaunchOutcome::Unavailable;
	return result;
#endif
}

std::string LoadPromptedVersion(const fs::path &configDir)
{
	if (configDir.empty())
		return std::string();
	std::ifstream file(configDir / kPrefsFile, std::ios::binary);
	if (!file)
		return std::string();
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const auto v = json::Parse(text);
	if (!v || !v->IsObject())
		return std::string();
	return v->GetString("promptedVersion").value_or("");
}

void SavePromptedVersion(const fs::path &configDir, const std::string &version)
{
	if (configDir.empty())
		return;
	json::Value v = json::Value::Object();
	v.Set("promptedVersion", json::Value::String(version));
	std::error_code ec;
	fs::create_directories(configDir, ec);
	std::ofstream file(configDir / kPrefsFile, std::ios::binary | std::ios::trunc);
	file << json::Serialize(v, true) << '\n';
}

} // namespace trigglow::updater
