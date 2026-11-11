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

// src/drive-kind.*: the delay buffer choice (Auto / Memory / Disk), its
// resolution against the drive type, the saved value, and the parts of drive
// detection that need no particular hardware. With folders as arguments it
// only prints what detection returns for each (a manual check on a machine):
//   drive_kind_tests C:\ D:\ /var/tmp

#include "check.hpp"
#include "drive-kind.hpp"

#include <cstdio>
#include <string>

using namespace trigglow::relay;

namespace {

void TestResolution()
{
	testcheck::g_currentTest = "resolution";
	// Automatic: Disk on an HDD only.
	CHECK(ResolveBufferStorage(BufferStorage::Auto, DriveKind::Ssd) == BufferStorage::Memory);
	CHECK(ResolveBufferStorage(BufferStorage::Auto, DriveKind::Hdd) == BufferStorage::Disk);
	CHECK(ResolveBufferStorage(BufferStorage::Auto, DriveKind::Unknown) == BufferStorage::Memory);
	// An explicit choice always wins.
	for (DriveKind drive : {DriveKind::Ssd, DriveKind::Hdd, DriveKind::Unknown}) {
		CHECK(ResolveBufferStorage(BufferStorage::Memory, drive) == BufferStorage::Memory);
		CHECK(ResolveBufferStorage(BufferStorage::Disk, drive) == BufferStorage::Disk);
		CHECK(SpillsToDisk(BufferStorage::Disk, drive));
		CHECK(!SpillsToDisk(BufferStorage::Memory, drive));
	}
	CHECK(SpillsToDisk(BufferStorage::Auto, DriveKind::Hdd));
	CHECK(!SpillsToDisk(BufferStorage::Auto, DriveKind::Ssd));
	CHECK(!SpillsToDisk(BufferStorage::Auto, DriveKind::Unknown));
}

void TestSavedValue()
{
	testcheck::g_currentTest = "saved value";
	CHECK(ParseBufferStorage("auto") == BufferStorage::Auto);
	CHECK(ParseBufferStorage("memory") == BufferStorage::Memory);
	CHECK(ParseBufferStorage("disk") == BufferStorage::Disk);
	CHECK(ParseBufferStorage("Disk") == BufferStorage::Disk);
	CHECK(ParseBufferStorage(" MEMORY ") == BufferStorage::Memory);
	// Settings from before the choice existed (no key), or a value this build
	// doesn't know: Automatic.
	CHECK(ParseBufferStorage("") == BufferStorage::Auto);
	CHECK(ParseBufferStorage("ram") == BufferStorage::Auto);
	CHECK(ParseBufferStorage("1") == BufferStorage::Auto);
	// What is saved reads back the same.
	for (BufferStorage s : {BufferStorage::Auto, BufferStorage::Memory, BufferStorage::Disk})
		CHECK(ParseBufferStorage(BufferStorageName(s)) == s);
	CHECK(std::string(BufferStorageName(BufferStorage::Auto)) == "auto");
	CHECK(std::string(DriveKindName(DriveKind::Ssd)) == "ssd");
	CHECK(std::string(DriveKindName(DriveKind::Hdd)) == "hdd");
	CHECK(std::string(DriveKindName(DriveKind::Unknown)) == "unknown");
}

void TestWindowsSignals()
{
	testcheck::g_currentTest = "windows signals";
	using drivekind::FromWindowsSignals;
	CHECK(FromWindowsSignals(1, -1) == DriveKind::Hdd);
	CHECK(FromWindowsSignals(1, 1) == DriveKind::Hdd); // Seek penalty wins (SMR drives can TRIM).
	CHECK(FromWindowsSignals(0, -1) == DriveKind::Ssd);
	CHECK(FromWindowsSignals(0, 0) == DriveKind::Ssd);
	CHECK(FromWindowsSignals(-1, 1) == DriveKind::Ssd);
	CHECK(FromWindowsSignals(-1, 0) == DriveKind::Unknown); // A bridge that drops TRIM proves nothing.
	CHECK(FromWindowsSignals(-1, -1) == DriveKind::Unknown);

	using drivekind::Combine;
	CHECK(Combine(DriveKind::Ssd, DriveKind::Ssd) == DriveKind::Ssd);
	CHECK(Combine(DriveKind::Ssd, DriveKind::Hdd) == DriveKind::Hdd);
	CHECK(Combine(DriveKind::Unknown, DriveKind::Hdd) == DriveKind::Hdd);
	CHECK(Combine(DriveKind::Ssd, DriveKind::Unknown) == DriveKind::Unknown);
	CHECK(Combine(DriveKind::Unknown, DriveKind::Unknown) == DriveKind::Unknown);
}

void TestLinux()
{
	testcheck::g_currentTest = "linux";
	using drivekind::FromRotational;
	CHECK(FromRotational("1\n") == DriveKind::Hdd);
	CHECK(FromRotational("0\n") == DriveKind::Ssd);
	CHECK(FromRotational("0") == DriveKind::Ssd);
	CHECK(FromRotational("") == DriveKind::Unknown);
	CHECK(FromRotational("2\n") == DriveKind::Unknown);
	CHECK(FromRotational("10") == DriveKind::Unknown);

	using drivekind::MountSourceFor;
	const std::string mountinfo =
		"22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
		"30 22 0:26 / /tmp rw,nosuid shared:5 - tmpfs tmpfs rw,size=8G\n"
		"31 22 8:17 / /var/tmp rw,relatime shared:6 - ext4 /dev/sdb1 rw\n"
		"40 22 0:35 /@home /home rw,relatime shared:7 - btrfs /dev/sda2 rw,subvol=/@home\n"
		"41 40 8:33 / /home/me/My\\040Videos rw - xfs /dev/sdc1 rw\n"
		"42 22 0:40 / /var rw - ext4 /dev/sdd1 rw\n"
		"43 22 0:41 / /var rw - ext4 /dev/sde1 rw\n"
		"garbage line\n";
	CHECK(MountSourceFor(mountinfo, "/") == "/dev/nvme0n1p2");
	CHECK(MountSourceFor(mountinfo, "/usr/lib") == "/dev/nvme0n1p2");
	CHECK(MountSourceFor(mountinfo, "/tmp") == "tmpfs");
	CHECK(MountSourceFor(mountinfo, "/var/tmp") == "/dev/sdb1");
	CHECK(MountSourceFor(mountinfo, "/var/tmp/x") == "/dev/sdb1");
	CHECK(MountSourceFor(mountinfo, "/var/tmpfoo") == "/dev/sde1"); // Not under /var/tmp; /var, mounted last.
	CHECK(MountSourceFor(mountinfo, "/home/me") == "/dev/sda2");
	CHECK(MountSourceFor(mountinfo, "/home/me/My Videos/a") == "/dev/sdc1");
	CHECK(MountSourceFor(mountinfo, "relative") == "");
	CHECK(MountSourceFor("", "/") == "");
}

void TestMac()
{
	testcheck::g_currentTest = "mac";
	using drivekind::FromMediumType;
	CHECK(FromMediumType("Solid State") == DriveKind::Ssd);
	CHECK(FromMediumType("Rotational") == DriveKind::Hdd);
	CHECK(FromMediumType("") == DriveKind::Unknown);
	CHECK(FromMediumType("solid state") == DriveKind::Unknown);
}

void TestDetectionDoesNotFail()
{
	testcheck::g_currentTest = "detection";
	// Whatever the machine's drives are, these must answer (and not crash).
	const DriveKind none = DetectDriveKind("");
	CHECK(none == DriveKind::Unknown);
	const DriveKind missing = DetectDriveKind("this/folder/does/not/exist/9f3c");
	(void)missing; // A relative path may still resolve to the current volume on Windows.
	const DriveKind here = DetectDriveKind(".");
	std::printf("drive_kind_tests: the current folder is on: %s\n", DriveKindName(here));
}

} // namespace

int main(int argc, char **argv)
{
	if (argc > 1) {
		for (int i = 1; i < argc; ++i)
			std::printf("%s: %s\n", argv[i], DriveKindName(DetectDriveKind(argv[i])));
		return 0;
	}
	TestResolution();
	TestSavedValue();
	TestWindowsSignals();
	TestLinux();
	TestMac();
	TestDetectionDoesNotFail();
	return testcheck::Finish("drive_kind_tests");
}
