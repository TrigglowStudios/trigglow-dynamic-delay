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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <thread>
#include <utility>
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

// ------------------------------------------------------------------ spill --

// A backend in memory that can be told to fail or be slow.
class MemBackend : public SpillBackend {
public:
	bool failWrites = false;
	bool failReads = false;
	int sleepMs = 0;
	int writes = 0;
	int reads = 0;
	int truncates = 0;
	uint64_t fileSlots = 0;
	std::map<uint64_t, std::pair<uint64_t, std::vector<uint8_t>>> slots; // slot -> blockId, bytes

	bool WriteSlot(uint64_t slot, uint64_t blockId, const uint8_t *data, size_t bytes) override
	{
		Nap();
		if (failWrites)
			return false;
		++writes;
		// "Encrypted" with the block id, so a mixed-up slot shows.
		std::vector<uint8_t> stored(data, data + bytes);
		for (uint8_t &b : stored)
			b = static_cast<uint8_t>(b ^ (blockId * 31 + 7));
		slots[slot] = {blockId, stored};
		fileSlots = std::max(fileSlots, slot + 1);
		return true;
	}
	bool ReadSlot(uint64_t slot, uint64_t blockId, uint8_t *data, size_t bytes) override
	{
		Nap();
		if (failReads)
			return false;
		++reads;
		auto it = slots.find(slot);
		if (it == slots.end() || it->second.second.size() != bytes)
			return false;
		for (size_t i = 0; i < bytes; ++i)
			data[i] = static_cast<uint8_t>(it->second.second[i] ^ (blockId * 31 + 7));
		return true;
	}
	bool Truncate(uint64_t keep) override
	{
		++truncates;
		for (auto it = slots.begin(); it != slots.end();)
			it = it->first >= keep ? slots.erase(it) : std::next(it);
		fileSlots = keep;
		return true;
	}

private:
	void Nap() const
	{
		if (sleepMs > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
	}
};

// The tests fill first and run the I/O after: no backlog limit.
PayloadStore::SpillLimits Roomy()
{
	PayloadStore::SpillLimits limits;
	limits.maxBacklogBlocks = 1 << 20;
	return limits;
}

// Runs the store's I/O until it has nothing to do (what the relay's I/O
// thread does, minus the lock). Returns the jobs run.
int Pump(PayloadStore &store, uint64_t ramFrom, const std::vector<PayloadStore::Range> &needed = {})
{
	int jobs = 0;
	for (; jobs < 100000; ++jobs) {
		PayloadStore::IoJob job = store.PlanIo(ramFrom, needed);
		if (job.kind == PayloadStore::IoJob::Kind::None)
			break;
		store.RunIo(job);
		store.FinishIo(job);
	}
	return jobs;
}

struct Fill {
	std::vector<std::pair<uint64_t, uint64_t>> records; // offset, number
	size_t Add(PayloadStore &store, uint64_t count, uint64_t first = 0)
	{
		size_t bytes = 0;
		for (uint64_t n = first; n < first + count; ++n) {
			const auto data = RecordBytes(n, SizeOf(n) / 16 + 10);
			records.emplace_back(store.Append(data.data(), data.size()), n);
			bytes += data.size();
		}
		return bytes;
	}
	bool AllMatch(const PayloadStore &store, uint64_t fromOffset = 0) const
	{
		for (const auto &r : records) {
			if (r.first < fromOffset)
				continue;
			const auto expected = RecordBytes(r.second, SizeOf(r.second) / 16 + 10);
			std::vector<uint8_t> got;
			if (!store.CopyOut(r.first, expected.size(), got) || got != expected)
				return false;
		}
		return true;
	}
};

void TestSpillRoundTrip()
{
	g_currentTest = "SpillRoundTrip";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	store.SetSpill(&disk, 1000, Roomy());
	CHECK(store.Spill() == SpillState::Active);
	Fill fill;
	fill.Add(store, 2000);
	const uint64_t end = store.EndOffset();
	// Keep the newest 8 blocks in RAM.
	const uint64_t ramFrom = end - 8 * block;
	Pump(store, ramFrom);
	CHECK(store.Spill() == SpillState::Active);
	CHECK(disk.writes > 0);
	// RAM: the window (8 blocks, 9 when it doesn't start on a block) + the
	// spare. Everything older is on disk.
	CHECK(store.RamBytes() <= 10 * block);
	const uint64_t onDisk = (ramFrom / block) * block - (store.FrontOffset() / block) * block;
	CHECK(store.DiskBytes() == onDisk);
	CHECK(!store.InRam(0, 10));
	CHECK(store.InRam(end - 100, 100));

	// A destination about to send the oldest bytes: they come back.
	const std::vector<PayloadStore::Range> needed = {{0, 3 * block + 5}};
	Pump(store, ramFrom, needed);
	CHECK(store.InRam(0, 3 * block + 5));
	CHECK(disk.reads == 4);
	CHECK(fill.AllMatch(store, ramFrom)); // RAM window intact.
	std::vector<uint8_t> got;
	bool head = true;
	for (const auto &r : fill.records) {
		if (r.first + 200 > 3 * block)
			break;
		const auto expected = RecordBytes(r.second, SizeOf(r.second) / 16 + 10);
		got.clear();
		if (!store.CopyOut(r.first, expected.size(), got) || got != expected)
			head = false;
	}
	CHECK(head);
	// Not needed any more: out of RAM again, no new write (already on disk).
	const int writes = disk.writes;
	Pump(store, ramFrom);
	CHECK(!store.InRam(0, 10));
	CHECK(disk.writes == writes);

	// Read every block back, in any order, and compare everything.
	const std::vector<PayloadStore::Range> all = {{0, end}};
	Pump(store, ramFrom, all);
	CHECK(fill.AllMatch(store));
}

void TestSpillTrimAndTruncate()
{
	g_currentTest = "SpillTrimAndTruncate";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	store.SetSpill(&disk, 1000, Roomy());
	Fill fill;
	fill.Add(store, 3000);
	Pump(store, store.EndOffset() - 4 * block);
	const uint64_t slotsBefore = disk.fileSlots;
	CHECK(slotsBefore > 20);
	CHECK(store.FileBytes() == slotsBefore * block);

	// Steady state: the front goes as much as comes in; freed slots are
	// reused and the file doesn't grow.
	for (int round = 0; round < 20; ++round) {
		fill.Add(store, 300, fill.records.size());
		const uint64_t keepFrom = store.EndOffset() - slotsBefore * block;
		size_t drop = 0;
		while (fill.records[drop].first < keepFrom)
			++drop;
		store.TrimTo(fill.records[drop].first);
		Pump(store, store.EndOffset() - 4 * block);
	}
	CHECK(disk.fileSlots <= slotsBefore + 6);
	CHECK(store.Spill() == SpillState::Active);
	CHECK(fill.AllMatch(store, store.EndOffset() - 4 * block));

	// The retention halves: once the blocks in the file's upper slots have
	// gone too, the file shrinks to what is still used.
	const int truncates = disk.truncates;
	const uint64_t half = store.EndOffset() - (slotsBefore / 2) * block;
	for (int round = 0; round < 40; ++round) {
		fill.Add(store, 150, fill.records.size());
		const uint64_t keepFrom = std::max(half, store.EndOffset() - (slotsBefore / 2) * block);
		size_t drop = 0;
		while (fill.records[drop].first < keepFrom)
			++drop;
		store.TrimTo(fill.records[drop].first);
		Pump(store, store.EndOffset() - 4 * block);
	}
	CHECK(disk.truncates > truncates);
	CHECK(disk.fileSlots <= slotsBefore / 2 + 6);
	CHECK(store.FileBytes() == disk.fileSlots * block);
	CHECK(store.DiskBytes() <= store.FileBytes());

	// Everything goes: so does the file.
	store.Clear();
	Pump(store, store.EndOffset());
	CHECK(disk.fileSlots == 0 && store.DiskBytes() == 0 && store.FileBytes() == 0);
}

void TestSpillWriteFailureFallsBack()
{
	g_currentTest = "SpillWriteFailureFallsBack";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	store.SetSpill(&disk, 1000, Roomy());
	Fill fill;
	fill.Add(store, 1500);
	Pump(store, store.EndOffset() - 4 * block);
	CHECK(store.DiskBytes() > 0);
	// The disk breaks: the spill stops, what was written comes back to RAM
	// and the rest stays there. Nothing is lost.
	disk.failWrites = true;
	fill.Add(store, 1500, 1500);
	Pump(store, store.EndOffset() - 4 * block);
	CHECK(store.Spill() == SpillState::Fallback);
	CHECK(store.SpillProblem() == "couldn't write to the disk");
	CHECK(store.LostBefore() == 0);
	CHECK(fill.AllMatch(store));
	CHECK(store.RamBytes() >= store.EndOffset() - store.FrontOffset());
	// More data: it simply stays in RAM.
	fill.Add(store, 500, 3000);
	CHECK(Pump(store, store.EndOffset() - 4 * block) == 0);
	CHECK(fill.AllMatch(store));
}

void TestSpillReadFailureLosesOnlyThatBlock()
{
	g_currentTest = "SpillReadFailureLosesOnlyThatBlock";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	store.SetSpill(&disk, 1000, Roomy());
	Fill fill;
	fill.Add(store, 1500);
	const uint64_t ramFrom = store.EndOffset() - 4 * block;
	Pump(store, ramFrom);
	disk.failReads = true;
	const std::vector<PayloadStore::Range> needed = {{block * 2 + 10, block * 2 + 20}};
	Pump(store, ramFrom, needed);
	CHECK(store.Spill() == SpillState::Fallback);
	CHECK(store.SpillProblem() == "couldn't read back from the disk");
	// Only up to the end of the block that failed is reported lost; the
	// fallback then tries the other blocks (also failing here, also lost).
	CHECK(store.LostBefore() >= 3 * block);
	CHECK(!store.InRam(block * 2 + 10, 10));
	// The newest bytes never left RAM.
	CHECK(store.InRam(ramFrom, store.EndOffset() - ramFrom));
}

void TestSpillSlowDiskFallsBack()
{
	g_currentTest = "SpillSlowDiskFallsBack";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	PayloadStore::SpillLimits limits = Roomy();
	limits.slowIoNs = 1000000; // 1 ms
	store.SetSpill(&disk, 1000, limits);
	disk.sleepMs = 5;
	Fill fill;
	fill.Add(store, 300);
	Pump(store, store.EndOffset() - 2 * block);
	CHECK(store.Spill() == SpillState::Fallback);
	CHECK(store.SpillProblem() == "the disk is too slow");
	CHECK(fill.AllMatch(store));
}

void TestSpillBacklogFallsBack()
{
	g_currentTest = "SpillBacklogFallsBack";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	PayloadStore::SpillLimits limits;
	limits.maxBacklogBlocks = 10;
	store.SetSpill(&disk, 1000, limits);
	Fill fill;
	fill.Add(store, 2000); // Far more than 10 blocks, none written yet.
	PayloadStore::IoJob job = store.PlanIo(store.EndOffset() - 2 * block, {});
	CHECK(job.kind == PayloadStore::IoJob::Kind::None);
	CHECK(store.Spill() == SpillState::Fallback);
	CHECK(store.SpillProblem() == "the disk can't keep up");
	CHECK(fill.AllMatch(store));
}

void TestSpillFullFallsBack()
{
	g_currentTest = "SpillFullFallsBack";
	const size_t block = 4096;
	PayloadStore store(block);
	MemBackend disk;
	store.SetSpill(&disk, 3, Roomy());
	Fill fill;
	fill.Add(store, 1000);
	Pump(store, store.EndOffset() - 2 * block);
	CHECK(store.Spill() == SpillState::Fallback);
	CHECK(store.SpillProblem() == "the disk space set aside for it is full");
	CHECK(disk.writes == 3);
	CHECK(fill.AllMatch(store));
}

void TestSpillOff()
{
	g_currentTest = "SpillOff";
	PayloadStore store(4096);
	CHECK(store.Spill() == SpillState::Off);
	Fill fill;
	fill.Add(store, 500);
	CHECK(store.PlanIo(store.EndOffset(), {}).kind == PayloadStore::IoJob::Kind::None);
	CHECK(fill.AllMatch(store));
}

} // namespace

int main()
{
	TestChunkedQueue();
	TestRoundTripAcrossBlocks();
	TestMultiPartRecord();
	TestTrimFreesBlocks();
	TestTrimAll();
	TestSpillOff();
	TestSpillRoundTrip();
	TestSpillTrimAndTruncate();
	TestSpillWriteFailureFallsBack();
	TestSpillReadFailureLosesOnlyThatBlock();
	TestSpillSlowDiskFallsBack();
	TestSpillBacklogFallsBack();
	TestSpillFullFallsBack();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
