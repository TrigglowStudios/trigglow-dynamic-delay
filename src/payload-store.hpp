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

// v1.0.0 stream mode: where the history's bytes live (packet-delay-core.hpp).
// Pure C++ -- no libobs, no FFmpeg, no Qt, no file or crypto API -- unit-tested
// in tests/payload_store_tests.cpp.
//
// Every packet the relay keeps is one "record" (its payload plus what the relay
// needs to rebuild it), appended back to back into large fixed-size blocks.
// A record has a logical offset that only grows; it may straddle two blocks.
// Trimming the history frees whole blocks from the front. Compared with one
// heap allocation per packet (FFmpeg's AVPacket + AVBufferRef + padding), the
// RAM is the bytes kept plus at most two partly used blocks, and large blocks
// go straight back to the system when freed (no heap fragmentation).
//
// Spill: with a SpillBackend (spill-file.hpp, an encrypted temp file), only the
// newest bytes and what each destination is about to send stay in RAM. Full
// blocks older than that are written out and their RAM freed; a block a sender
// will need again is read back before it is due. The I/O runs on one thread
// that doesn't hold the caller's lock (PlanIo / RunIo / FinishIo). When the
// disk fails, fills up or is too slow, the spill stops and everything stays in
// RAM again (blocks already written are read back); nothing is dropped unless
// a block can't be read back at all (LostBefore()).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace trigglow::relay {

// A FIFO of small structs stored in fixed chunks (no per-element allocation,
// no reallocation of what is already there; chunks are freed as the front
// moves). Indexing is O(1).
template<typename T, size_t ChunkSize = 4096> class ChunkedQueue {
public:
	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }

	T &operator[](size_t i)
	{
		const size_t at = head_ + i;
		return chunks_[at / ChunkSize][at % ChunkSize];
	}
	const T &operator[](size_t i) const
	{
		const size_t at = head_ + i;
		return chunks_[at / ChunkSize][at % ChunkSize];
	}
	T &front() { return (*this)[0]; }
	const T &front() const { return (*this)[0]; }
	T &back() { return (*this)[size_ - 1]; }
	const T &back() const { return (*this)[size_ - 1]; }

	void push_back(const T &value)
	{
		const size_t at = head_ + size_;
		if (at / ChunkSize >= chunks_.size())
			chunks_.push_back(std::make_unique<T[]>(ChunkSize));
		chunks_[at / ChunkSize][at % ChunkSize] = value;
		++size_;
	}

	// Drops the first `count` elements.
	void pop_front(size_t count = 1)
	{
		if (count >= size_) {
			clear();
			return;
		}
		head_ += count;
		size_ -= count;
		while (head_ >= ChunkSize) {
			chunks_.pop_front();
			head_ -= ChunkSize;
		}
	}

	void clear()
	{
		chunks_.clear();
		head_ = 0;
		size_ = 0;
	}

	// RAM held by the chunks.
	size_t CapacityBytes() const { return chunks_.size() * ChunkSize * sizeof(T); }

private:
	std::deque<std::unique_ptr<T[]>> chunks_;
	size_t head_ = 0;
	size_t size_ = 0;
};

// Where blocks that leave RAM go. Slots are block-sized places in a file;
// `blockId` (the block's number in the store, never reused) is what the
// encryption counter is derived from. Called from one thread at a time.
class SpillBackend {
public:
	virtual ~SpillBackend() = default;
	virtual bool WriteSlot(uint64_t slot, uint64_t blockId, const uint8_t *data, size_t bytes) = 0;
	virtual bool ReadSlot(uint64_t slot, uint64_t blockId, uint8_t *data, size_t bytes) = 0;
	// Keeps only the first `slots` slots.
	virtual bool Truncate(uint64_t slots) = 0;
};

enum class SpillState {
	Off,      // No backend: everything in RAM.
	Active,   // Old blocks go to the backend.
	Fallback, // The backend failed, filled up or was too slow: back to RAM only.
};

class PayloadStore {
public:
	static constexpr size_t kDefaultBlockBytes = size_t{1} << 20;

	struct Span {
		const uint8_t *data = nullptr;
		size_t bytes = 0;
	};

	// A byte range [from, to) of the store.
	struct Range {
		uint64_t from = 0;
		uint64_t to = 0;
	};

	// When the spill gives up on a slow disk.
	struct SpillLimits {
		// One block written or read in more than this.
		int64_t slowIoNs = 2000000000LL;
		// More than this many full blocks waiting to be written.
		size_t maxBacklogBlocks = 48;
		// Free slots at the end of the file before it is truncated.
		uint64_t truncateSlackSlots = 4;
	};

	struct IoJob {
		enum class Kind { None, Write, Read, Truncate };
		Kind kind = Kind::None;
		uint64_t blockId = 0;
		uint64_t slot = 0; // Truncate: the slots kept.
		std::shared_ptr<uint8_t[]> data;
		bool ok = false;
		int64_t elapsedNs = 0;
	};

	explicit PayloadStore(size_t blockBytes = kDefaultBlockBytes);

	// Appends the pieces back to back as one record; returns its offset.
	uint64_t Append(const Span *parts, size_t count);
	uint64_t Append(const uint8_t *data, size_t bytes)
	{
		const Span span{data, bytes};
		return Append(&span, 1);
	}

	// True when [offset, offset + bytes) is in RAM now.
	bool InRam(uint64_t offset, size_t bytes) const;
	// Copies [offset, offset + bytes) to the end of `out`. False (and `out`
	// unchanged) when part of it isn't kept any more or isn't in RAM now.
	bool CopyOut(uint64_t offset, size_t bytes, std::vector<uint8_t> &out) const;

	// Frees every block that lies entirely before `offset`.
	void TrimTo(uint64_t offset);
	// Drops every byte (offsets keep growing). The spill stays as it was.
	void Clear();

	uint64_t FrontOffset() const { return frontOffset_; }
	uint64_t EndOffset() const { return endOffset_; }
	size_t BlockBytes() const { return blockBytes_; }
	// RAM held for bytes (blocks in RAM, the spare, the job in flight).
	size_t RamBytes() const;

	// --- Spill ---

	// Starts spilling to `backend` (not owned; must outlive the store's I/O),
	// using at most `maxSlots` slots. nullptr: RAM only.
	void SetSpill(SpillBackend *backend, uint64_t maxSlots, SpillLimits limits);
	void SetSpill(SpillBackend *backend, uint64_t maxSlots) { SetSpill(backend, maxSlots, SpillLimits{}); }
	SpillState Spill() const { return state_; }
	// Why the spill fell back to RAM (empty otherwise).
	const std::string &SpillProblem() const { return problem_; }
	// Bytes in the backend's slots (in use), and the file's length.
	uint64_t DiskBytes() const { return usedSlots_ * blockBytes_; }
	uint64_t FileBytes() const { return fileSlots_ * blockBytes_; }

	// Under the caller's lock: frees the RAM of blocks safely on disk that are
	// neither at or after `ramFrom` (the newest bytes) nor in `needed` (what
	// destinations are about to send), and returns the next I/O to run
	// (Kind::None: nothing to do now). Needed reads come first.
	IoJob PlanIo(uint64_t ramFrom, const std::vector<Range> &needed);
	// Without the lock, one job at a time: runs it on the backend.
	void RunIo(IoJob &job) const;
	// Under the lock: applies the job's result.
	void FinishIo(IoJob &job);
	// Bytes before this offset may be lost (a block couldn't be read back):
	// the history drops the packets that start before it.
	uint64_t LostBefore() const { return lostBefore_; }

private:
	struct Block {
		std::shared_ptr<uint8_t[]> data; // nullptr: on disk only (or lost).
		int64_t slot = -1;               // >= 0: a copy is in this slot.
		bool busy = false;               // A job is running on it.
	};

	std::shared_ptr<uint8_t[]> NewBlockData();
	void Recycle(std::shared_ptr<uint8_t[]> &data);
	Block *Find(uint64_t blockId);
	bool Needed(uint64_t blockId, const std::vector<Range> &needed) const;
	bool AllocateSlot(uint64_t &slot);
	void FreeSlot(int64_t slot);
	void Fail(const std::string &problem);

	size_t blockBytes_;
	// blocks_[i] holds logical bytes [(firstBlock_ + i) * blockBytes_, +blockBytes_).
	std::deque<Block> blocks_;
	uint64_t firstBlock_ = 0;
	uint64_t frontOffset_ = 0;
	uint64_t endOffset_ = 0;
	// One freed block kept for the next append or read: no allocate/free
	// churn at the block boundary in steady state.
	std::shared_ptr<uint8_t[]> spare_;

	SpillBackend *backend_ = nullptr;
	SpillLimits limits_;
	SpillState state_ = SpillState::Off;
	std::string problem_;
	uint64_t maxSlots_ = 0;
	uint64_t slotEnd_ = 0;   // Slots [0, slotEnd_) have been handed out at some point.
	uint64_t usedSlots_ = 0; // Slots holding a block (or being written).
	uint64_t fileSlots_ = 0; // The file's length, in slots.
	std::set<uint64_t> freeSlots_;
	uint64_t lostBefore_ = 0;
	bool jobInFlight_ = false;
	size_t inFlightBytes_ = 0;
};

} // namespace trigglow::relay
