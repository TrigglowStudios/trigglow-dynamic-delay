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

// v1.0.0 stream mode: the encrypted temp file the history spills to
// (payload-store.hpp SpillBackend). No libobs, no FFmpeg, no Qt; unit-tested in
// tests/spill_file_tests.cpp.
//
// - AES-256 in counter mode with a random key and nonce made for this file and
//   kept only in this process's memory: what is on disk is unreadable without
//   them, and they are gone when the stream stops. The counter for a block is
//   derived from its number in the store, which is never reused, so no
//   keystream is used twice even when a slot is. The AES itself is the
//   system's: Windows CNG (bcrypt), CommonCrypto on macOS, OpenSSL on Linux
//   (which the Linux build already links for RTMPS). No new dependency.
// - The file lives in the system's temp folder (on Linux not a RAM-backed
//   tmpfs when a disk-backed one exists), is deleted when closed -- also when
//   the process dies (Windows: delete-on-close; elsewhere it is unlinked right
//   after it is created) -- and RemoveStale() deletes any a power loss left.
// - Integrity isn't checked (no MAC): the file is only ever read back by the
//   process that wrote it, in the same session.

#include "payload-store.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace trigglow::relay {

// False when this build has no AES to use (the spill then stays off).
bool SpillCryptoAvailable();

// XORs `data` with the AES-256-CTR keystream for `key`, starting at the
// 128-bit big-endian `counter` (incremented by one per 16 bytes). Encrypts and
// decrypts. Exposed for the tests' known-answer check.
bool AesCtrXor(const uint8_t key[32], const uint8_t counter[16], uint8_t *data, size_t bytes);

class SpillFile final : public SpillBackend {
public:
	// Creates the file in `directory` (empty: the system's temp folder).
	// nullptr and a short reason when it can't.
	static std::unique_ptr<SpillFile> Create(size_t blockBytes, std::string &error,
						 const std::string &directory = {});
	~SpillFile() override;
	SpillFile(const SpillFile &) = delete;
	SpillFile &operator=(const SpillFile &) = delete;

	bool WriteSlot(uint64_t slot, uint64_t blockId, const uint8_t *data, size_t bytes) override;
	bool ReadSlot(uint64_t slot, uint64_t blockId, uint8_t *data, size_t bytes) override;
	bool Truncate(uint64_t slots) override;

	// The file's current length in bytes.
	uint64_t SizeOnDisk() const;
	// The bytes of a slot as they are on disk (encrypted), for the tests.
	bool ReadRawSlot(uint64_t slot, uint8_t *data, size_t bytes);
	const std::string &Path() const { return path_; }

	// The folder the spill uses by default.
	static std::string TempDirectory();
	// Free bytes on the volume of `directory` (empty: the default), 0 unknown.
	static uint64_t FreeDiskBytes(const std::string &directory = {});
	// Deletes spill files left behind (a crash before they could be deleted);
	// files another running OBS still has open are left alone. Returns how
	// many were deleted.
	static int RemoveStale(const std::string &directory = {});

private:
	struct Impl;
	SpillFile(std::unique_ptr<Impl> impl, size_t blockBytes, std::string path);
	bool Crypt(uint64_t blockId, size_t offsetInBlock, const uint8_t *in, uint8_t *out, size_t bytes);

	std::unique_ptr<Impl> impl_;
	size_t blockBytes_;
	std::string path_;
};

} // namespace trigglow::relay
