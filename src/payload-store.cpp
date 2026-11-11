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
#include <cstring>

namespace trigglow::relay {

PayloadStore::PayloadStore(size_t blockBytes) : blockBytes_(std::max<size_t>(blockBytes, 64)) {}

std::shared_ptr<uint8_t[]> PayloadStore::NewBlockData()
{
	if (spare_)
		return std::move(spare_);
	return std::shared_ptr<uint8_t[]>(new uint8_t[blockBytes_]);
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
			while (block >= firstBlock_ + blocks_.size())
				blocks_.push_back(Block{NewBlockData()});
			const size_t n = std::min(left, blockBytes_ - within);
			std::memcpy(blocks_[static_cast<size_t>(block - firstBlock_)].data.get() + within, src, n);
			src += n;
			left -= n;
			endOffset_ += n;
		}
	}
	return start;
}

bool PayloadStore::CopyOut(uint64_t offset, size_t bytes, std::vector<uint8_t> &out) const
{
	if (offset < frontOffset_ || offset + bytes > endOffset_)
		return false;
	const size_t base = out.size();
	out.resize(base + bytes);
	uint8_t *dst = out.data() + base;
	while (bytes > 0) {
		const uint64_t block = offset / blockBytes_;
		const size_t within = static_cast<size_t>(offset % blockBytes_);
		const Block &b = blocks_[static_cast<size_t>(block - firstBlock_)];
		if (!b.data) {
			out.resize(base);
			return false;
		}
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
		if (!spare_ && blocks_.front().data && blocks_.front().data.use_count() == 1)
			spare_ = std::move(blocks_.front().data);
		blocks_.pop_front();
		++firstBlock_;
	}
}

void PayloadStore::Clear()
{
	blocks_.clear();
	frontOffset_ = endOffset_;
	firstBlock_ = endOffset_ / blockBytes_;
	spare_.reset();
}

size_t PayloadStore::RamBytes() const
{
	size_t bytes = spare_ ? blockBytes_ : 0;
	for (const Block &b : blocks_) {
		if (b.data)
			bytes += blockBytes_;
	}
	return bytes;
}

} // namespace trigglow::relay
