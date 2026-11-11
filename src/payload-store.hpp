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
// Pure C++ -- no libobs, no FFmpeg, no Qt -- unit-tested in
// tests/payload_store_tests.cpp.
//
// Every packet the relay keeps is one "record" (its payload plus what the relay
// needs to rebuild it), appended back to back into large fixed-size blocks.
// A record has a logical offset that only grows; it may straddle two blocks.
// Trimming the history frees whole blocks from the front. Compared with one
// heap allocation per packet (FFmpeg's AVPacket + AVBufferRef + padding), the
// RAM is the bytes kept plus at most two partly used blocks, and large blocks
// go straight back to the system when freed (no heap fragmentation).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
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

class PayloadStore {
public:
	static constexpr size_t kDefaultBlockBytes = size_t{1} << 20;

	struct Span {
		const uint8_t *data = nullptr;
		size_t bytes = 0;
	};

	explicit PayloadStore(size_t blockBytes = kDefaultBlockBytes);

	// Appends the pieces back to back as one record; returns its offset.
	uint64_t Append(const Span *parts, size_t count);
	uint64_t Append(const uint8_t *data, size_t bytes)
	{
		const Span span{data, bytes};
		return Append(&span, 1);
	}

	// Copies [offset, offset + bytes) to the end of `out`. False (and `out`
	// unchanged) when part of it isn't kept any more.
	bool CopyOut(uint64_t offset, size_t bytes, std::vector<uint8_t> &out) const;

	// Frees every block that lies entirely before `offset`.
	void TrimTo(uint64_t offset);
	void Clear();

	uint64_t FrontOffset() const { return frontOffset_; }
	uint64_t EndOffset() const { return endOffset_; }
	size_t BlockBytes() const { return blockBytes_; }
	// RAM held for bytes (blocks in use plus the spare).
	size_t RamBytes() const;

private:
	struct Block {
		std::shared_ptr<uint8_t[]> data;
	};

	std::shared_ptr<uint8_t[]> NewBlockData();

	size_t blockBytes_;
	// blocks_[i] holds logical bytes [(firstBlock_ + i) * blockBytes_, +blockBytes_).
	std::deque<Block> blocks_;
	uint64_t firstBlock_ = 0;
	uint64_t frontOffset_ = 0;
	uint64_t endOffset_ = 0;
	// One freed block kept for the next append: no allocate/free churn at
	// the block boundary in steady state.
	std::shared_ptr<uint8_t[]> spare_;
};

} // namespace trigglow::relay
