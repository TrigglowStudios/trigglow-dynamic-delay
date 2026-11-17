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

// Unit tests for trigglow::relay::SpillFile (src/spill-file.hpp): the stream
// mode history's encrypted temp file, with real files in the system's temp
// folder and the system's AES. No libobs, no FFmpeg, no framework. Without
// AES in the build (Linux without OpenSSL) only the "unavailable" path is
// checked.

#include "spill-file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace trigglow::relay;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_currentTest = "";

void Check(bool ok, const char *expr, const char *file, int line)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("FAIL [%s] %s:%d: %s\n", g_currentTest, file, line, expr);
	}
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

std::vector<uint8_t> Hex(const char *hex)
{
	std::vector<uint8_t> out;
	auto nibble = [](char c) {
		return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
	};
	for (; hex[0] && hex[1]; hex += 2)
		out.push_back(static_cast<uint8_t>(nibble(hex[0]) * 16 + nibble(hex[1])));
	return out;
}

std::vector<uint8_t> Pattern(uint64_t seed, size_t size)
{
	std::vector<uint8_t> bytes(size);
	uint32_t x = static_cast<uint32_t>(seed * 2654435761u + 99u);
	for (size_t i = 0; i < size; ++i) {
		x = x * 1664525u + 1013904223u;
		bytes[i] = static_cast<uint8_t>(x >> 24);
	}
	return bytes;
}

bool Exists(const std::string &path)
{
#ifdef _WIN32
	const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
	return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
#endif
}

// Counts how many of `plain`'s 16-byte blocks appear unchanged in `stored`.
size_t PlainBlocksIn(const std::vector<uint8_t> &plain, const std::vector<uint8_t> &stored)
{
	size_t same = 0;
	for (size_t i = 0; i + 16 <= plain.size(); i += 16) {
		if (std::equal(plain.begin() + static_cast<std::ptrdiff_t>(i),
			       plain.begin() + static_cast<std::ptrdiff_t>(i + 16),
			       stored.begin() + static_cast<std::ptrdiff_t>(i)))
			++same;
	}
	return same;
}

// NIST SP 800-38A, F.5.5 CTR-AES256.Encrypt.
void TestAesCtrKnownAnswer()
{
	g_currentTest = "AesCtrKnownAnswer";
	const auto key = Hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
	const auto counter = Hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
	auto data = Hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
			"30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
	const auto expected = Hex("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
				  "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6");
	CHECK(AesCtrXor(key.data(), counter.data(), data.data(), data.size()));
	CHECK(data == expected);
	// The counter carries across 64 bits (f..ff -> 00..00 with a carry).
	const auto carry = Hex("00000000000000ffffffffffffffffff");
	std::vector<uint8_t> a(32, 0);
	CHECK(AesCtrXor(key.data(), carry.data(), a.data(), a.size()));
	const auto next = Hex("00000000000001000000000000000000");
	std::vector<uint8_t> b(16, 0);
	CHECK(AesCtrXor(key.data(), next.data(), b.data(), b.size()));
	CHECK(std::equal(b.begin(), b.end(), a.begin() + 16));
}

void TestRoundTrip()
{
	g_currentTest = "RoundTrip";
	const size_t block = 256 * 1024;
	std::string error;
	auto file = SpillFile::Create(block, error);
	CHECK(file != nullptr);
	if (!file) {
		std::printf("  (%s)\n", error.c_str());
		return;
	}
	// Slots out of order, block ids that only grow, a slot overwritten.
	const uint64_t slots[5] = {3, 0, 7, 1, 3};
	const uint64_t ids[5] = {10, 11, 12, 13, 14};
	for (int i = 0; i < 5; ++i) {
		const auto plain = Pattern(ids[i], block);
		CHECK(file->WriteSlot(slots[i], ids[i], plain.data(), plain.size()));
	}
	for (int i = 1; i < 5; ++i) {
		std::vector<uint8_t> got(block);
		CHECK(file->ReadSlot(slots[i], ids[i], got.data(), got.size()));
		CHECK(got == Pattern(ids[i], block));
	}
	CHECK(file->SizeOnDisk() == 8 * block);
	// Read with the wrong block id: garbage, not the data (the counter is
	// tied to the block).
	std::vector<uint8_t> wrong(block);
	CHECK(file->ReadSlot(7, 13, wrong.data(), wrong.size()));
	CHECK(wrong != Pattern(12, block));
	// Past the end of the file: refused.
	std::vector<uint8_t> past(block);
	CHECK(!file->ReadSlot(20, 15, past.data(), past.size()));
}

void TestEncryptedAtRest()
{
	g_currentTest = "EncryptedAtRest";
	const size_t block = 64 * 1024;
	std::string error;
	auto one = SpillFile::Create(block, error);
	auto two = SpillFile::Create(block, error);
	CHECK(one && two);
	if (!one || !two)
		return;
	CHECK(one->Path() != two->Path());
	const std::vector<uint8_t> zeros(block, 0);
	const auto text = Pattern(5, block);
	CHECK(one->WriteSlot(0, 1, zeros.data(), block));
	CHECK(one->WriteSlot(1, 2, zeros.data(), block));
	CHECK(one->WriteSlot(2, 3, text.data(), block));
	CHECK(two->WriteSlot(0, 1, zeros.data(), block));
	std::vector<uint8_t> raw0(block), raw1(block), raw2(block), rawOther(block);
	CHECK(one->ReadRawSlot(0, raw0.data(), block));
	CHECK(one->ReadRawSlot(1, raw1.data(), block));
	CHECK(one->ReadRawSlot(2, raw2.data(), block));
	CHECK(two->ReadRawSlot(0, rawOther.data(), block));
	// Nothing of the plaintext is on disk.
	CHECK(PlainBlocksIn(zeros, raw0) == 0);
	CHECK(PlainBlocksIn(text, raw2) == 0);
	// Same bytes, different block: different ciphertext (no repeated
	// keystream). Same bytes, same block id, another file: another key.
	CHECK(raw0 != raw1);
	CHECK(raw0 != rawOther);
	// The ciphertext looks random: roughly half the bits set.
	size_t ones = 0;
	for (uint8_t b : raw0) {
		for (int bit = 0; bit < 8; ++bit)
			ones += (b >> bit) & 1;
	}
	const double ratio = static_cast<double>(ones) / (block * 8.0);
	CHECK(ratio > 0.49 && ratio < 0.51);
}

void TestTruncate()
{
	g_currentTest = "Truncate";
	const size_t block = 64 * 1024;
	std::string error;
	auto file = SpillFile::Create(block, error);
	CHECK(file != nullptr);
	if (!file)
		return;
	const auto plain = Pattern(1, block);
	for (uint64_t slot = 0; slot < 10; ++slot)
		CHECK(file->WriteSlot(slot, 100 + slot, plain.data(), block));
	CHECK(file->SizeOnDisk() == 10 * block);
	CHECK(file->Truncate(4));
	CHECK(file->SizeOnDisk() == 4 * block);
	std::vector<uint8_t> got(block);
	CHECK(file->ReadSlot(3, 103, got.data(), block) && got == plain);
	CHECK(!file->ReadSlot(5, 105, got.data(), block));
	CHECK(file->Truncate(0));
	CHECK(file->SizeOnDisk() == 0);
}

void TestDeletedOnClose()
{
	g_currentTest = "DeletedOnClose";
	std::string error;
	auto file = SpillFile::Create(4096, error);
	CHECK(file != nullptr);
	if (!file)
		return;
	const std::string path = file->Path();
	CHECK(path.find(SpillFile::TempDirectory()) == 0);
#ifdef _WIN32
	// Visible while open (delete-on-close), but nobody else can open it.
	CHECK(Exists(path));
	const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
	HANDLE other = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				   nullptr, OPEN_EXISTING, 0, nullptr);
	CHECK(other == INVALID_HANDLE_VALUE);
	if (other != INVALID_HANDLE_VALUE)
		CloseHandle(other);
	// A running session's file isn't "stale".
	SpillFile::RemoveStale();
	CHECK(Exists(path));
#else
	// Unlinked right after it was created.
	CHECK(!Exists(path));
#endif
	file.reset();
	CHECK(!Exists(path));
}

void TestRemoveStale()
{
	g_currentTest = "RemoveStale";
	const std::string dir = SpillFile::TempDirectory();
	// What a power loss would leave: a spill file nobody has open.
	const std::string stale = dir + "/trigglow-dd-spill-1-00000000deadbeef.tmp";
	const std::string other = dir + "/trigglow-dd-other-test.tmp";
	for (const std::string &path : {stale, other}) {
		std::FILE *f = std::fopen(path.c_str(), "wb");
		CHECK(f != nullptr);
		if (f) {
			std::fputs("left behind", f);
			std::fclose(f);
		}
	}
	CHECK(Exists(stale));
	CHECK(SpillFile::RemoveStale() >= 1);
	CHECK(!Exists(stale));
	// Only spill files are touched.
	CHECK(Exists(other));
	std::remove(other.c_str());
}

void TestCreateFailsCleanly()
{
	g_currentTest = "CreateFailsCleanly";
	std::string error;
	auto file = SpillFile::Create(4096, error, SpillFile::TempDirectory() + "/no-such-folder-trigglow/x");
	CHECK(file == nullptr);
	CHECK(!error.empty());
	error.clear();
	CHECK(SpillFile::Create(1000, error) == nullptr); // Not a multiple of 16.
	CHECK(!error.empty());
	CHECK(SpillFile::FreeDiskBytes() > 0);
}

// The store and the file together, the way the relay's I/O thread runs them.
void TestStoreOnFile()
{
	g_currentTest = "StoreOnFile";
	const size_t block = 64 * 1024;
	std::string error;
	auto file = SpillFile::Create(block, error);
	CHECK(file != nullptr);
	if (!file)
		return;
	PayloadStore store(block);
	PayloadStore::SpillLimits limits;
	limits.maxBacklogBlocks = 1 << 20;
	store.SetSpill(file.get(), 10000, limits);
	std::vector<std::pair<uint64_t, std::vector<uint8_t>>> records;
	for (uint64_t n = 0; n < 400; ++n) {
		auto data = Pattern(n, 1000 + static_cast<size_t>(n * 7919 % 30000));
		records.emplace_back(store.Append(data.data(), data.size()), std::move(data));
	}
	auto pump = [&](uint64_t ramFrom, const std::vector<PayloadStore::Range> &needed) {
		for (;;) {
			PayloadStore::IoJob job = store.PlanIo(ramFrom, needed);
			if (job.kind == PayloadStore::IoJob::Kind::None)
				return;
			store.RunIo(job);
			store.FinishIo(job);
		}
	};
	const uint64_t ramFrom = store.EndOffset() - 3 * block;
	pump(ramFrom, {});
	CHECK(store.Spill() == SpillState::Active);
	CHECK(store.DiskBytes() > 80 * block);
	CHECK(store.RamBytes() <= 5 * block);
	CHECK(file->SizeOnDisk() == store.FileBytes());
	pump(ramFrom, {{0, store.EndOffset()}});
	bool same = true;
	for (const auto &r : records) {
		std::vector<uint8_t> got;
		if (!store.CopyOut(r.first, r.second.size(), got) || got != r.second)
			same = false;
	}
	CHECK(same);
	store.Clear();
	pump(store.EndOffset(), {});
	CHECK(file->SizeOnDisk() == 0);
}

} // namespace

int main()
{
	if (!SpillCryptoAvailable()) {
		g_currentTest = "Unavailable";
		std::string error;
		CHECK(SpillFile::Create(4096, error) == nullptr);
		CHECK(!error.empty());
		std::printf("spill_file_tests: no AES in this build, encryption checks skipped\n");
	} else {
		TestAesCtrKnownAnswer();
		TestRoundTrip();
		TestEncryptedAtRest();
		TestTruncate();
		TestDeletedOnClose();
		TestRemoveStale();
		TestCreateFailsCleanly();
		TestStoreOnFile();
	}
	std::printf("spill_file_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
