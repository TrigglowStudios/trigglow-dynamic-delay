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

#include "drive-kind.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <climits>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace trigglow::relay {

const char *DriveKindName(DriveKind kind)
{
	switch (kind) {
	case DriveKind::Ssd:
		return "ssd";
	case DriveKind::Hdd:
		return "hdd";
	case DriveKind::Unknown:
		break;
	}
	return "unknown";
}

BufferStorage ParseBufferStorage(const std::string &value)
{
	std::string lower;
	lower.reserve(value.size());
	for (char c : value) {
		if (!std::isspace(static_cast<unsigned char>(c)))
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "memory")
		return BufferStorage::Memory;
	if (lower == "disk")
		return BufferStorage::Disk;
	return BufferStorage::Auto;
}

const char *BufferStorageName(BufferStorage storage)
{
	switch (storage) {
	case BufferStorage::Memory:
		return "memory";
	case BufferStorage::Disk:
		return "disk";
	case BufferStorage::Auto:
		break;
	}
	return "auto";
}

BufferStorage ResolveBufferStorage(BufferStorage choice, DriveKind drive)
{
	if (choice != BufferStorage::Auto)
		return choice;
	return drive == DriveKind::Hdd ? BufferStorage::Disk : BufferStorage::Memory;
}

namespace drivekind {

DriveKind FromWindowsSignals(int incursSeekPenalty, int trimEnabled)
{
	if (incursSeekPenalty == 1)
		return DriveKind::Hdd;
	if (incursSeekPenalty == 0)
		return DriveKind::Ssd;
	return trimEnabled == 1 ? DriveKind::Ssd : DriveKind::Unknown;
}

DriveKind Combine(DriveKind a, DriveKind b)
{
	if (a == DriveKind::Hdd || b == DriveKind::Hdd)
		return DriveKind::Hdd;
	if (a == DriveKind::Ssd && b == DriveKind::Ssd)
		return DriveKind::Ssd;
	return DriveKind::Unknown;
}

DriveKind FromRotational(const std::string &content)
{
	size_t i = 0;
	while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i])))
		++i;
	size_t end = i;
	while (end < content.size() && !std::isspace(static_cast<unsigned char>(content[end])))
		++end;
	const std::string value = content.substr(i, end - i);
	if (value == "1")
		return DriveKind::Hdd;
	if (value == "0")
		return DriveKind::Ssd;
	return DriveKind::Unknown;
}

namespace {

// mountinfo escapes space, tab, newline and backslash as \ooo.
std::string DecodeMountField(const std::string &field)
{
	std::string out;
	out.reserve(field.size());
	for (size_t i = 0; i < field.size(); ++i) {
		if (field[i] == '\\' && i + 3 < field.size()) {
			const char a = field[i + 1], b = field[i + 2], c = field[i + 3];
			if (a >= '0' && a <= '3' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
				out.push_back(static_cast<char>((a - '0') * 64 + (b - '0') * 8 + (c - '0')));
				i += 3;
				continue;
			}
		}
		out.push_back(field[i]);
	}
	return out;
}

bool PathUnder(const std::string &path, const std::string &mountPoint)
{
	if (mountPoint == "/")
		return !path.empty() && path[0] == '/';
	if (path.size() < mountPoint.size() || path.compare(0, mountPoint.size(), mountPoint) != 0)
		return false;
	return path.size() == mountPoint.size() || path[mountPoint.size()] == '/';
}

} // namespace

std::string MountSourceFor(const std::string &mountinfoText, const std::string &path)
{
	std::istringstream lines(mountinfoText);
	std::string line;
	std::string best;
	size_t bestLength = 0;
	bool found = false;
	while (std::getline(lines, line)) {
		std::istringstream fields(line);
		std::vector<std::string> tokens;
		std::string token;
		while (fields >> token)
			tokens.push_back(token);
		// id parent major:minor root mountpoint options [optional...] - fstype source superoptions
		if (tokens.size() < 7)
			continue;
		const std::string mountPoint = DecodeMountField(tokens[4]);
		size_t dash = 6;
		while (dash < tokens.size() && tokens[dash] != "-")
			++dash;
		if (dash + 2 >= tokens.size())
			continue;
		if (!PathUnder(path, mountPoint))
			continue;
		// Deepest mount point wins; of equal ones the last (mounted on top).
		if (!found || mountPoint.size() >= bestLength) {
			best = DecodeMountField(tokens[dash + 2]);
			bestLength = mountPoint.size();
			found = true;
		}
	}
	return best;
}

DriveKind FromMediumType(const std::string &mediumType)
{
	if (mediumType == "Solid State")
		return DriveKind::Ssd;
	if (mediumType == "Rotational")
		return DriveKind::Hdd;
	return DriveKind::Unknown;
}

} // namespace drivekind

#if defined(_WIN32)

namespace {

std::wstring Widen(const std::string &text)
{
	if (text.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (n <= 0)
		return {};
	std::wstring out(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
	return out;
}

class Handle {
public:
	explicit Handle(HANDLE h) : h_(h) {}
	~Handle()
	{
		if (valid())
			CloseHandle(h_);
	}
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	bool valid() const { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
	HANDLE get() const { return h_; }

private:
	HANDLE h_;
};

// No access rights asked for: the storage property queries need none, so this
// works without administrator rights.
HANDLE OpenDevice(const std::wstring &path)
{
	return CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

int QuerySeekPenalty(HANDLE device)
{
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceSeekPenaltyProperty;
	query.QueryType = PropertyStandardQuery;
	DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = {};
	DWORD got = 0;
	if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
			     sizeof(descriptor), &got, nullptr) ||
	    got < sizeof(descriptor))
		return -1;
	return descriptor.IncursSeekPenalty ? 1 : 0;
}

int QueryTrim(HANDLE device)
{
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceTrimProperty;
	query.QueryType = PropertyStandardQuery;
	DEVICE_TRIM_DESCRIPTOR descriptor = {};
	DWORD got = 0;
	if (!DeviceIoControl(device, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
			     sizeof(descriptor), &got, nullptr) ||
	    got < sizeof(descriptor))
		return -1;
	return descriptor.TrimEnabled ? 1 : 0;
}

DriveKind QueryDevice(HANDLE device)
{
	return drivekind::FromWindowsSignals(QuerySeekPenalty(device), QueryTrim(device));
}

} // namespace

DriveKind DetectDriveKind(const std::string &folder)
{
	const std::wstring wide = Widen(folder);
	if (wide.empty())
		return DriveKind::Unknown;
	wchar_t volumePath[MAX_PATH + 1] = {};
	if (!GetVolumePathNameW(wide.c_str(), volumePath, MAX_PATH + 1))
		return DriveKind::Unknown;
	// Network shares, RAM disks, optical drives: nothing to ask.
	const UINT type = GetDriveTypeW(volumePath);
	if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
		return DriveKind::Unknown;
	wchar_t volumeName[MAX_PATH + 1] = {};
	if (!GetVolumeNameForVolumeMountPointW(volumePath, volumeName, MAX_PATH + 1))
		return DriveKind::Unknown;
	std::wstring volume(volumeName);
	while (!volume.empty() && volume.back() == L'\\')
		volume.pop_back();
	const Handle volumeHandle(OpenDevice(volume));
	if (!volumeHandle.valid())
		return DriveKind::Unknown;

	// The physical disk(s) behind the volume.
	constexpr DWORD kMaxExtents = 16;
	std::vector<unsigned char> buffer(sizeof(VOLUME_DISK_EXTENTS) + kMaxExtents * sizeof(DISK_EXTENT));
	DWORD got = 0;
	if (DeviceIoControl(volumeHandle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(),
			    static_cast<DWORD>(buffer.size()), &got, nullptr)) {
		const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buffer.data());
		const DWORD count = std::min<DWORD>(extents->NumberOfDiskExtents, kMaxExtents);
		std::vector<DWORD> seen;
		DriveKind result = DriveKind::Unknown;
		for (DWORD i = 0; i < count; ++i) {
			const DWORD disk = extents->Extents[i].DiskNumber;
			if (std::find(seen.begin(), seen.end(), disk) != seen.end())
				continue;
			const Handle diskHandle(OpenDevice(L"\\\\.\\PhysicalDrive" + std::to_wstring(disk)));
			const DriveKind kind = diskHandle.valid() ? QueryDevice(diskHandle.get()) : DriveKind::Unknown;
			result = seen.empty() ? kind : drivekind::Combine(result, kind);
			seen.push_back(disk);
		}
		if (!seen.empty())
			return result;
	}
	// No extents (some virtual volumes): the volume may pass the query on.
	return QueryDevice(volumeHandle.get());
}

#elif defined(__APPLE__)

DriveKind DetectDriveKind(const std::string &folder)
{
	struct statfs fs = {};
	if (folder.empty() || statfs(folder.c_str(), &fs) != 0)
		return DriveKind::Unknown;
	std::string bsd = fs.f_mntfromname;
	if (bsd.rfind("/dev/", 0) == 0)
		bsd = bsd.substr(5);
	if (bsd.rfind("disk", 0) != 0)
		return DriveKind::Unknown; // Network or synthetic file system.

	// MACH_PORT_NULL is the default main port (kIOMainPortDefault on newer SDKs).
	CFMutableDictionaryRef matching = IOBSDNameMatching(MACH_PORT_NULL, 0, bsd.c_str());
	if (!matching)
		return DriveKind::Unknown;
	const io_service_t media = IOServiceGetMatchingService(MACH_PORT_NULL, matching); // Consumes `matching`.
	if (!media)
		return DriveKind::Unknown;
	// Up the registry: the APFS volume, its container, the physical store, the
	// block storage device that carries the characteristics.
	CFTypeRef characteristics = IORegistryEntrySearchCFProperty(
		media, kIOServicePlane, CFSTR("Device Characteristics"), kCFAllocatorDefault,
		kIORegistryIterateRecursively | kIORegistryIterateParents);
	IOObjectRelease(media);
	if (!characteristics)
		return DriveKind::Unknown;
	DriveKind kind = DriveKind::Unknown;
	if (CFGetTypeID(characteristics) == CFDictionaryGetTypeID()) {
		const void *medium =
			CFDictionaryGetValue(static_cast<CFDictionaryRef>(characteristics), CFSTR("Medium Type"));
		if (medium && CFGetTypeID(medium) == CFStringGetTypeID()) {
			char text[64] = {};
			if (CFStringGetCString(static_cast<CFStringRef>(medium), text, sizeof(text),
					       kCFStringEncodingUTF8))
				kind = drivekind::FromMediumType(text);
		}
	}
	CFRelease(characteristics);
	return kind;
}

#else

namespace {

std::string ReadSmallFile(const std::string &path)
{
	std::ifstream in(path);
	if (!in)
		return {};
	std::string content;
	std::getline(in, content);
	return content;
}

DriveKind FromDeviceNumber(dev_t device)
{
	const std::string base =
		"/sys/dev/block/" + std::to_string(major(device)) + ":" + std::to_string(minor(device));
	std::string content = ReadSmallFile(base + "/queue/rotational");
	if (content.empty()) // A partition: its disk has the queue.
		content = ReadSmallFile(base + "/../queue/rotational");
	return drivekind::FromRotational(content);
}

} // namespace

DriveKind DetectDriveKind(const std::string &folder)
{
	if (folder.empty())
		return DriveKind::Unknown;
	char resolved[PATH_MAX] = {};
	const std::string path = realpath(folder.c_str(), resolved) ? std::string(resolved) : folder;
	struct stat st = {};
	if (stat(path.c_str(), &st) != 0)
		return DriveKind::Unknown;
	if (major(st.st_dev) != 0) {
		const DriveKind kind = FromDeviceNumber(st.st_dev);
		if (kind != DriveKind::Unknown)
			return kind;
	}
	// Anonymous device numbers (btrfs, overlay...): the mount's source device.
	std::ifstream in("/proc/self/mountinfo");
	if (!in)
		return DriveKind::Unknown;
	std::stringstream text;
	text << in.rdbuf();
	const std::string source = drivekind::MountSourceFor(text.str(), path);
	struct stat dev = {};
	if (source.rfind("/dev/", 0) != 0 || stat(source.c_str(), &dev) != 0 || !S_ISBLK(dev.st_mode))
		return DriveKind::Unknown;
	return FromDeviceNumber(dev.st_rdev);
}

#endif

} // namespace trigglow::relay
