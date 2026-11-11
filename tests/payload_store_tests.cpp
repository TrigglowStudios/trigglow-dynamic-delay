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

// Standalone unit tests for trigglow::relay::PayloadStore and ChunkedQueue
// (src/payload-store.hpp): the stream mode history's contiguous byte store.
// Same approach as the other tests: no libobs, no FFmpeg, no framework.

#include "payload-store.hpp"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

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

// Deterministic bytes for record `n`, `size` long.
std::vector<uint8_t> RecordBytes(uint64_t n, size_t size)
{
	std::vector<uint8_t> bytes(size);
	uint32_t x = static_cast<uint32_t>(n * 2654435761u + 12345u);
	for (size_t i = 0; i < size; ++i) {
		x = x * 1664525u + 1013904223u;
		bytes[i] = static_cast<uint8_t>(x >> 24);
	}
	return bytes;
}

// Record sizes like a stream's: mostly small, a big one now and then.
size_t SizeOf(uint64_t n)
{
	if (n % 60 == 0)
		return 150000 + static_cast<size_t>(n % 7) * 1000; // Keyframe.
	if (n % 3 == 0)
		return 380; // Audio.
	return 9000 + static_cast<size_t>(n % 13) * 700;
}

void TestChunkedQueue()
{
	g_currentTest = "ChunkedQueue";
	ChunkedQueue<uint64_t, 16> q;
	CHECK(q.empty());
	for (uint64_t i = 0; i < 100; ++i)
		q.push_back(i);
	CHECK(q.size() == 100);
	CHECK(q.front() == 0 && q.back() == 99 && q[37] == 37);
	q.pop_front(40);
	CHECK(q.size() == 60 && q.front() == 40 && q[59] == 99);
	// The chunks before the front were freed: 60 elements from index 40
	// span chunks 2..6 of 16 elements.
	CHECK(q.CapacityBytes() <= 5 * 16 * sizeof(uint64_t));
	for (uint64_t i = 100; i < 1000; ++i) {
		q.push_back(i);
		q.pop_front();
	}
	CHECK(q.size() == 60 && q.front() == 940 && q.back() == 999);
	CHECK(q.CapacityBytes() <= 5 * 16 * sizeof(uint64_t));
	q.pop_front(1000);
	CHECK(q.empty() && q.CapacityBytes() == 0);
	q.push_back(7);
	CHECK(q.size() == 1 && q.front() == 7);
	q.clear();
	CHECK(q.empty());
}

void TestRoundTripAcrossBlocks()
{
	g_currentTest = "RoundTripAcrossBlocks";
	PayloadStore store(4096);
	std::vector<uint64_t> offsets;
	for (uint64_t n = 0; n < 200; ++n) {
		const auto bytes = RecordBytes(n, 100 + static_cast<size_t>(n * 37 % 5000));
		offsets.push_back(store.Append(bytes.data(), bytes.size()));
	}
	bool same = true;
	for (uint64_t n = 0; n < 200; ++n) {
		const auto expected = RecordBytes(n, 100 + static_cast<size_t>(n * 37 % 5000));
		std::vector<uint8_t> got;
		if (!store.CopyOut(offsets[n], expected.size(), got) || got != expected)
			same = false;
	}
	CHECK(same);
	// Records straddle blocks (sizes above the block) and pack back to back.
	CHECK(offsets[1] == offsets[0] + 100);
	CHECK(store.EndOffset() == offsets.back() + 100 + 199 * 37 % 5000);
}

void TestMultiPartRecord()
{
	g_currentTest = "MultiPartRecord";
	PayloadStore store(64);
	const uint8_t header[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	const auto side = RecordBytes(1, 30);
	const auto payload = RecordBytes(2, 200);
	const PayloadStore::Span parts[3] = {{header, 8}, {side.data(), side.size()}, {payload.data(), payload.size()}};
	const uint64_t at = store.Append(parts, 3);
	std::vector<uint8_t> got = {9}; // CopyOut appends.
	CHECK(store.CopyOut(at, 238, got));
	CHECK(got.size() == 239 && got[0] == 9 && got[1] == 1 && got[8] == 8);
	CHECK(std::vector<uint8_t>(got.begin() + 9, got.begin() + 39) == side);
	CHECK(std::vector<uint8_t>(got.begin() + 39, got.end()) == payload);
	// Past the end: refused, `out` unchanged.
	std::vector<uint8_t> none;
	CHECK(!store.CopyOut(at, 239, none) && none.empty());
	// Empty parts.
	const uint64_t empty = store.Append(static_cast<const PayloadStore::Span *>(nullptr), 0);
	CHECK(empty == at + 238 && store.EndOffset() == empty);
}

void TestTrimFreesBlocks()
{
	g_currentTest = "TrimFreesBlocks";
	const size_t block = 1 << 16;
	PayloadStore store(block);
	std::deque<std::pair<uint64_t, uint64_t>> kept; // offset, record number
	const size_t keepBytes = 3 * 1024 * 1024;
	size_t keptBytes = 0;
	size_t worstOverhead = 0;
	bool same = true;
	for (uint64_t n = 0; n < 20000; ++n) {
		const auto bytes = RecordBytes(n, SizeOf(n));
		kept.emplace_back(store.Append(bytes.data(), bytes.size()), n);
		keptBytes += bytes.size();
		while (keptBytes > keepBytes) {
			keptBytes -= SizeOf(kept.front().second);
			kept.pop_front();
		}
		store.TrimTo(kept.front().first);
		const size_t ram = store.RamBytes();
		if (ram > keptBytes)
			worstOverhead = std::max(worstOverhead, ram - keptBytes);
		if (n % 997 == 0) {
			for (const auto &k : kept) {
				const auto expected = RecordBytes(k.second, SizeOf(k.second));
				std::vector<uint8_t> got;
				if (!store.CopyOut(k.first, expected.size(), got) || got != expected)
					same = false;
			}
		}
	}
	CHECK(same);
	// RAM = the bytes kept + at most the two partly used blocks + the spare.
	CHECK(worstOverhead <= 3 * block);
	// What was trimmed is gone.
	std::vector<uint8_t> got;
	CHECK(!store.CopyOut(0, 10, got));
	CHECK(store.FrontOffset() == kept.front().first);

	store.Clear();
	CHECK(store.RamBytes() == 0);
	CHECK(store.FrontOffset() == store.EndOffset());
	// Offsets keep growing after a Clear.
	const auto bytes = RecordBytes(1, 10);
	const uint64_t after = store.Append(bytes.data(), bytes.size());
	CHECK(after == store.FrontOffset());
	CHECK(store.CopyOut(after, 10, got) && got == bytes);
}

void TestTrimAll()
{
	g_currentTest = "TrimAll";
	PayloadStore store(256);
	const auto bytes = RecordBytes(3, 1000);
	store.Append(bytes.data(), bytes.size());
	store.TrimTo(store.EndOffset());
	// Only the last, partly used block (appends go on there) and the spare.
	CHECK(store.RamBytes() <= 2 * 256);
	store.TrimTo(0); // Backwards: ignored.
	CHECK(store.FrontOffset() == store.EndOffset());
	const uint64_t at = store.Append(bytes.data(), bytes.size());
	std::vector<uint8_t> got;
	CHECK(store.CopyOut(at, bytes.size(), got) && got == bytes);
}

} // namespace

int main()
{
	TestChunkedQueue();
	TestRoundTripAcrossBlocks();
	TestMultiPartRecord();
	TestTrimFreesBlocks();
	TestTrimAll();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
