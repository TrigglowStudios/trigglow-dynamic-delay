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

// v1.0.0 stream mode: where the delay buffer (the relay's packet history)
// lives, and the drive type its Automatic choice is based on. No libobs, no
// Qt, no FFmpeg; unit-tested in tests/drive_kind_tests.cpp.
//
// - Memory: the whole history stays in RAM (more RAM, nothing written to
//   disk). It reaches back the delay, the highest preset or delay-on scene
//   rule, plus the splice slack (packet-delay-core.hpp KeepSeconds).
// - Disk: the older history goes to the encrypted temp file (spill-file.hpp);
//   RAM stays small and every live raise is instant, but the whole stream is
//   written to the disk (~2.7 GB per hour at 6 Mbps). Falls back to RAM when
//   the disk fails.
// - Automatic: Memory on an SSD (no continuous writes wearing it) or when the
//   drive type can't be told (network drive, a USB bridge that won't say, an
//   error); Disk on a spinning hard drive, where writes cost no wear and RAM
//   is usually the scarcer thing.
//
// The drive is the one holding the spill folder (SpillFile::TempDirectory()):
// - Windows: the volume's disk(s), IOCTL_STORAGE_QUERY_PROPERTY with
//   StorageDeviceSeekPenaltyProperty, StorageDeviceTrimProperty as a second
//   signal when the first can't be read.
// - Linux: /sys/dev/block/<major:minor>/queue/rotational of the device behind
//   the folder (its parent disk for a partition; the mount's source device
//   from /proc/self/mountinfo for filesystems like btrfs with an anonymous
//   device number).
// - macOS: IOKit's "Device Characteristics" / "Medium Type" ("Solid State",
//   "Rotational") up the registry from the folder's BSD disk.

#include <string>

namespace trigglow::relay {

enum class DriveKind { Unknown = 0, Ssd = 1, Hdd = 2 };

// The streamer's choice ("buffer_storage" in stream-mode-settings.json).
enum class BufferStorage { Auto = 0, Memory = 1, Disk = 2 };

// The drive type of the volume `folder` is on. Unknown when it can't be told.
// Touches the system (a few ioctls / sysfs reads); call it at stream start or
// when the dock needs it, not per packet.
DriveKind DetectDriveKind(const std::string &folder);

// "ssd", "hdd", "unknown" (logs).
const char *DriveKindName(DriveKind kind);

// The saved value: "auto", "memory", "disk" (any case). Missing, empty or
// unknown values (settings from before the choice existed, a hand-edited
// file) are Auto.
BufferStorage ParseBufferStorage(const std::string &value);
// What is saved: "auto", "memory", "disk".
const char *BufferStorageName(BufferStorage storage);

// What a stream uses: Memory or Disk (never Auto). Auto is Disk on an HDD
// only; SSD and Unknown are Memory.
BufferStorage ResolveBufferStorage(BufferStorage choice, DriveKind drive);
inline bool SpillsToDisk(BufferStorage choice, DriveKind drive)
{
	return ResolveBufferStorage(choice, drive) == BufferStorage::Disk;
}

// The pieces of detection that need no hardware (exposed for the tests).
namespace drivekind {

// Windows: the disk's two answers, each -1 (couldn't be read), 0 or 1. A seek
// penalty is a spinning disk; no seek penalty is solid state. Without it, TRIM
// means solid state; no TRIM alone proves nothing (USB bridges and older
// drivers don't pass it on), so it is Unknown.
DriveKind FromWindowsSignals(int incursSeekPenalty, int trimEnabled);
// Several disks behind one volume (spanned, striped, Storage Spaces): any
// spinning one makes it an HDD; all solid state an SSD; otherwise Unknown.
DriveKind Combine(DriveKind a, DriveKind b);

// Linux: the content of queue/rotational ("1\n" spinning, "0\n" not).
DriveKind FromRotational(const std::string &content);
// Linux: the mount source ("/dev/nvme0n1p2") of the deepest mount point in
// /proc/self/mountinfo `text` that contains `path` (absolute, no trailing
// slash except for "/"). Mount points' octal escapes (\040 for a space) are
// decoded. Empty when none matches.
std::string MountSourceFor(const std::string &mountinfoText, const std::string &path);

// macOS: the "Medium Type" string of the device characteristics.
DriveKind FromMediumType(const std::string &mediumType);

} // namespace drivekind

} // namespace trigglow::relay
