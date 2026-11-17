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

#include "payload-store.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace trigglow::relay {

PayloadStore::PayloadStore(size_t blockBytes) : blockBytes_(std::max<size_t>(blockBytes, 64)) {}

std::shared_ptr<uint8_t[]> PayloadStore::NewBlockData()
{
	if (spare_)
		return std::move(spare_);
	return std::shared_ptr<uint8_t[]>(new uint8_t[blockBytes_]);
}

void PayloadStore::Recycle(std::shared_ptr<uint8_t[]> &data)
{
	if (!spare_ && data && data.use_count() == 1)
		spare_ = std::move(data);
	data.reset();
}

PayloadStore::Block *PayloadStore::Find(uint64_t blockId)
{
	if (blockId < firstBlock_ || blockId >= firstBlock_ + blocks_.size())
		return nullptr;
	return &blocks_[static_cast<size_t>(blockId - firstBlock_)];
}

uint64_t PayloadStore::Append(const Span *parts, size_t count)
{
	const uint64_t start = endOffset_;
	for (size_t i = 0; i < count; ++i) {
		const uint8_t *src = parts[i].data;
		size_t left = parts[i].bytes;
		while (left > 0) {
			const uint64_t block = endOffset_ / blockBytes_;
			const size_t within = static_cast<size_t>(endOffset_ % blockBytes_);
			if (blocks_.empty())
				firstBlock_ = block;
			while (block >= firstBlock_ + blocks_.size()) {
				Block fresh;
				fresh.data = NewBlockData();
				blocks_.push_back(std::move(fresh));
			}
			const size_t n = std::min(left, blockBytes_ - within);
			std::memcpy(blocks_[static_cast<size_t>(block - firstBlock_)].data.get() + within, src, n);
			src += n;
			left -= n;
			endOffset_ += n;
		}
	}
	return start;
}

bool PayloadStore::InRam(uint64_t offset, size_t bytes) const
{
	if (offset < frontOffset_ || offset + bytes > endOffset_)
		return false;
	if (bytes == 0)
		return true;
	const uint64_t last = (offset + bytes - 1) / blockBytes_;
	for (uint64_t block = offset / blockBytes_; block <= last; ++block) {
		if (!blocks_[static_cast<size_t>(block - firstBlock_)].data)
			return false;
	}
	return true;
}

bool PayloadStore::CopyOut(uint64_t offset, size_t bytes, std::vector<uint8_t> &out) const
{
	if (!InRam(offset, bytes))
		return false;
	const size_t base = out.size();
	out.resize(base + bytes);
	uint8_t *dst = out.data() + base;
	while (bytes > 0) {
		const uint64_t block = offset / blockBytes_;
		const size_t within = static_cast<size_t>(offset % blockBytes_);
		const Block &b = blocks_[static_cast<size_t>(block - firstBlock_)];
		const size_t n = std::min(bytes, blockBytes_ - within);
		std::memcpy(dst, b.data.get() + within, n);
		dst += n;
		offset += n;
		bytes -= n;
	}
	return true;
}

void PayloadStore::TrimTo(uint64_t offset)
{
	offset = std::min(offset, endOffset_);
	if (offset <= frontOffset_)
		return;
	frontOffset_ = offset;
	while (!blocks_.empty() && (firstBlock_ + 1) * blockBytes_ <= frontOffset_) {
		Block &b = blocks_.front();
		// A block being written has no slot yet (FinishIo frees the job's);
		// one being read keeps its slot until then, and no other job can
		// reuse it meanwhile (one job at a time).
		FreeSlot(b.slot);
		Recycle(b.data);
		blocks_.pop_front();
		++firstBlock_;
	}
}

void PayloadStore::Clear()
{
	for (Block &b : blocks_)
		FreeSlot(b.slot);
	blocks_.clear();
	frontOffset_ = endOffset_;
	firstBlock_ = endOffset_ / blockBytes_;
	spare_.reset();
}

size_t PayloadStore::RamBytes() const
{
	size_t bytes = (spare_ ? blockBytes_ : 0) + inFlightBytes_;
	for (const Block &b : blocks_) {
		if (b.data)
			bytes += blockBytes_;
	}
	return bytes;
}

// ------------------------------------------------------------------ spill --

void PayloadStore::SetSpill(SpillBackend *backend, uint64_t maxSlots, SpillLimits limits)
{
	backend_ = backend;
	maxSlots_ = maxSlots;
	limits_ = limits;
	state_ = backend ? SpillState::Active : SpillState::Off;
	problem_.clear();
}

void PayloadStore::Fail(const std::string &problem)
{
	if (state_ != SpillState::Active)
		return;
	state_ = SpillState::Fallback;
	problem_ = problem;
}

bool PayloadStore::AllocateSlot(uint64_t &slot)
{
	if (!freeSlots_.empty()) {
		slot = *freeSlots_.begin();
		freeSlots_.erase(freeSlots_.begin());
	} else if (slotEnd_ < maxSlots_) {
		slot = slotEnd_++;
	} else {
		return false;
	}
	++usedSlots_;
	return true;
}

void PayloadStore::FreeSlot(int64_t slot)
{
	if (slot < 0)
		return;
	freeSlots_.insert(static_cast<uint64_t>(slot));
	if (usedSlots_ > 0)
		--usedSlots_;
	// The slots at the end of the file are no longer handed out: the file
	// can shrink to what is still used.
	while (slotEnd_ > 0) {
		auto last = freeSlots_.find(slotEnd_ - 1);
		if (last == freeSlots_.end())
			break;
		freeSlots_.erase(last);
		--slotEnd_;
	}
}

bool PayloadStore::Needed(uint64_t blockId, const std::vector<Range> &needed) const
{
	const uint64_t start = blockId * blockBytes_;
	const uint64_t end = start + blockBytes_;
	for (const Range &r : needed) {
		if (r.from < end && r.to > start)
			return true;
	}
	return false;
}

PayloadStore::IoJob PayloadStore::PlanIo(uint64_t ramFrom, const std::vector<Range> &needed)
{
	IoJob job;
	if (!backend_ || state_ == SpillState::Off || jobInFlight_)
		return job;
	const bool active = state_ == SpillState::Active;

	// 1. Free the RAM of blocks that are safely on disk and not wanted now.
	if (active) {
		for (size_t i = 0; i < blocks_.size(); ++i) {
			Block &b = blocks_[i];
			const uint64_t id = firstBlock_ + i;
			if (!b.data || b.busy || b.slot < 0 || (id + 1) * blockBytes_ > ramFrom || Needed(id, needed))
				continue;
			Recycle(b.data);
		}
	}

	auto startRead = [&](uint64_t id, Block &b) {
		b.busy = true;
		job.kind = IoJob::Kind::Read;
		job.blockId = id;
		job.slot = static_cast<uint64_t>(b.slot);
		job.data = NewBlockData();
		inFlightBytes_ = blockBytes_;
		jobInFlight_ = true;
		return job;
	};

	// 2. Read back what a destination is about to send (in the order given).
	for (const Range &r : needed) {
		if (r.to <= r.from)
			continue;
		const uint64_t first = std::max(r.from, frontOffset_) / blockBytes_;
		const uint64_t last = (r.to - 1) / blockBytes_;
		for (uint64_t id = first; id <= last; ++id) {
			Block *b = Find(id);
			if (b && !b->data && b->slot >= 0 && !b->busy)
				return startRead(id, *b);
		}
	}
	// RAM only again: everything still on disk comes back, oldest first.
	if (!active) {
		for (size_t i = 0; i < blocks_.size(); ++i) {
			Block &b = blocks_[i];
			if (!b.data && b.slot >= 0 && !b.busy)
				return startRead(firstBlock_ + i, b);
		}
	}

	// 3. Write out the oldest full block that is older than the RAM window.
	if (active) {
		size_t backlog = 0;
		size_t oldest = blocks_.size();
		for (size_t i = 0; i < blocks_.size(); ++i) {
			const Block &b = blocks_[i];
			const uint64_t end = (firstBlock_ + i + 1) * blockBytes_;
			if (!b.data || b.busy || b.slot >= 0 || end > endOffset_ || end > ramFrom)
				continue;
			if (oldest == blocks_.size())
				oldest = i;
			++backlog;
		}
		if (backlog > limits_.maxBacklogBlocks) {
			Fail("the disk can't keep up");
			return PlanIo(ramFrom, needed);
		}
		if (oldest < blocks_.size()) {
			uint64_t slot = 0;
			if (!AllocateSlot(slot)) {
				Fail("the disk space set aside for it is full");
				return PlanIo(ramFrom, needed);
			}
			Block &b = blocks_[oldest];
			b.busy = true;
			job.kind = IoJob::Kind::Write;
			job.blockId = firstBlock_ + oldest;
			job.slot = slot;
			job.data = b.data;
			jobInFlight_ = true;
			return job;
		}
	}

	// 4. Give back the disk the history no longer uses.
	if (fileSlots_ > slotEnd_ && (fileSlots_ - slotEnd_ >= limits_.truncateSlackSlots || slotEnd_ == 0)) {
		job.kind = IoJob::Kind::Truncate;
		job.slot = slotEnd_;
		jobInFlight_ = true;
		return job;
	}
	return job;
}

void PayloadStore::RunIo(IoJob &job) const
{
	const auto start = std::chrono::steady_clock::now();
	switch (job.kind) {
	case IoJob::Kind::Write:
		job.ok = backend_ && backend_->WriteSlot(job.slot, job.blockId, job.data.get(), blockBytes_);
		break;
	case IoJob::Kind::Read:
		job.ok = backend_ && backend_->ReadSlot(job.slot, job.blockId, job.data.get(), blockBytes_);
		break;
	case IoJob::Kind::Truncate:
		job.ok = backend_ && backend_->Truncate(job.slot);
		break;
	case IoJob::Kind::None:
		job.ok = true;
		break;
	}
	job.elapsedNs = static_cast<int64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

void PayloadStore::FinishIo(IoJob &job)
{
	if (job.kind == IoJob::Kind::None)
		return;
	jobInFlight_ = false;
	inFlightBytes_ = 0;
	Block *b = job.kind == IoJob::Kind::Truncate ? nullptr : Find(job.blockId);
	switch (job.kind) {
	case IoJob::Kind::Write:
		if (b)
			b->busy = false;
		if (job.ok) {
			fileSlots_ = std::max(fileSlots_, job.slot + 1);
			if (b)
				b->slot = static_cast<int64_t>(job.slot);
			else
				FreeSlot(static_cast<int64_t>(job.slot)); // Trimmed meanwhile.
		} else {
			FreeSlot(static_cast<int64_t>(job.slot));
			Fail("couldn't write to the disk");
		}
		break;
	case IoJob::Kind::Read:
		if (!b) {
			Recycle(job.data); // Trimmed meanwhile.
			break;
		}
		b->busy = false;
		if (job.ok) {
			b->data = std::move(job.data);
		} else {
			// The block is gone: its bytes exist nowhere else.
			Recycle(job.data);
			FreeSlot(b->slot);
			b->slot = -1;
			lostBefore_ = std::max(lostBefore_, (job.blockId + 1) * blockBytes_);
			Fail("couldn't read back from the disk");
		}
		break;
	case IoJob::Kind::Truncate:
		if (job.ok)
			fileSlots_ = std::max(job.slot, slotEnd_);
		else
			Fail("couldn't shrink its file");
		break;
	case IoJob::Kind::None:
		break;
	}
	if (job.ok && job.elapsedNs > limits_.slowIoNs)
		Fail("the disk is too slow");
	job.data.reset();
}

} // namespace trigglow::relay
