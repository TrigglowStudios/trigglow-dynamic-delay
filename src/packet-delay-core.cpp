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

#include "packet-delay-core.hpp"

#include <algorithm>

namespace trigglow::relay {

namespace {
bool IsVideoKey(const Packet &p)
{
	return p.kind == MediaKind::Video && p.keyframe;
}
} // namespace

// ---------------------------------------------------------------- history --

uint64_t PacketHistory::Push(Packet packet, const PayloadStore::Span *parts, size_t partCount)
{
	const uint64_t seq = nextSeq_++;
	if (packets_.empty())
		frontSeq_ = seq;
	if (packet.kind == MediaKind::Video)
		sawVideo_ = true;
	size_t recordBytes = 0;
	for (size_t i = 0; i < partCount; ++i)
		recordBytes += parts[i].bytes;
	packet.recordOffset = store_.Append(parts, partCount);
	packet.recordBytes = static_cast<uint32_t>(recordBytes);
	bytes_ += packet.bytes;
	packets_.push_back(packet);
	return seq;
}

void PacketHistory::Trim(int64_t nowNs, int64_t retentionNs, uint64_t minKeepSeq)
{
	if (packets_.empty())
		return;
	const int64_t cutoff = nowNs - retentionNs;
	size_t drop = 0;
	if (sawVideo_) {
		// Drop up to the newest keyframe that is itself older than the cutoff:
		// the GOPs before it are entirely older, and what is kept still starts
		// at a keyframe.
		for (size_t i = 1; i < packets_.size(); ++i) {
			const Packet &p = packets_[i];
			if (p.arrivalNs > cutoff || frontSeq_ + i > minKeepSeq)
				break;
			if (IsVideoKey(p))
				drop = i;
		}
	} else {
		while (drop < packets_.size() && packets_[drop].arrivalNs <= cutoff && frontSeq_ + drop < minKeepSeq)
			++drop;
	}
	for (size_t i = 0; i < drop; ++i)
		bytes_ -= packets_[i].bytes;
	packets_.pop_front(drop);
	frontSeq_ += drop;
	store_.TrimTo(packets_.empty() ? store_.EndOffset() : packets_.front().recordOffset);
}

bool PacketHistory::FindSplicePoint(int64_t atNs, uint64_t &seqOut) const
{
	if (packets_.empty())
		return false;
	if (!sawVideo_) {
		bool haveOldestAudio = false;
		size_t oldestAudio = 0;
		for (size_t i = packets_.size(); i-- > 0;) {
			if (packets_[i].arrivalNs < spliceFloorNs_)
				break;
			if (packets_[i].arrivalNs <= atNs) {
				seqOut = frontSeq_ + i;
				return true;
			}
			oldestAudio = i;
			haveOldestAudio = true;
		}
		if (!haveOldestAudio)
			return false;
		seqOut = frontSeq_ + oldestAudio;
		return true;
	}
	bool haveOldest = false;
	size_t oldest = 0;
	for (size_t i = packets_.size(); i-- > 0;) {
		const Packet &p = packets_[i];
		if (p.arrivalNs < spliceFloorNs_)
			break;
		if (!IsVideoKey(p))
			continue;
		if (p.arrivalNs <= atNs) {
			seqOut = frontSeq_ + i;
			return true;
		}
		oldest = i;
		haveOldest = true;
	}
	if (!haveOldest)
		return false;
	seqOut = frontSeq_ + oldest;
	return true;
}

uint64_t PacketHistory::OffsetAtArrival(int64_t atNs) const
{
	// Arrival times never go down: binary search.
	size_t lo = 0;
	size_t hi = packets_.size();
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (packets_[mid].arrivalNs < atNs)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < packets_.size() ? packets_[lo].recordOffset : store_.EndOffset();
}

PayloadStore::Range PacketHistory::AheadRange(uint64_t seq, int64_t aheadNs) const
{
	const Packet *first = At(seq);
	if (!first)
		return {};
	const int64_t until = first->arrivalNs + aheadNs;
	// The last packet that arrived at or before `until`.
	size_t lo = static_cast<size_t>(seq - frontSeq_);
	size_t hi = packets_.size();
	while (lo + 1 < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (packets_[mid].arrivalNs <= until)
			lo = mid;
		else
			hi = mid;
	}
	const Packet &last = packets_[lo];
	return PayloadStore::Range{first->recordOffset, last.recordOffset + last.recordBytes};
}

void PacketHistory::DropLost()
{
	const uint64_t lost = store_.LostBefore();
	if (packets_.empty() || packets_.front().recordOffset >= lost)
		return;
	size_t drop = 0;
	while (drop < packets_.size() && packets_[drop].recordOffset < lost)
		++drop;
	// What is kept still starts at a keyframe.
	if (sawVideo_) {
		while (drop < packets_.size() && !IsVideoKey(packets_[drop]))
			++drop;
	}
	for (size_t i = 0; i < drop; ++i)
		bytes_ -= packets_[i].bytes;
	packets_.pop_front(drop);
	frontSeq_ += drop;
	store_.TrimTo(packets_.empty() ? store_.EndOffset() : packets_.front().recordOffset);
}

const Packet *PacketHistory::At(uint64_t seq) const
{
	if (seq < frontSeq_ || seq >= EndSeq())
		return nullptr;
	return &packets_[static_cast<size_t>(seq - frontSeq_)];
}

double PacketHistory::SpanSeconds() const
{
	if (packets_.size() < 2)
		return 0.0;
	return static_cast<double>(packets_.back().arrivalNs - packets_.front().arrivalNs) / 1e9;
}

double PacketHistory::KeyframeIntervalSeconds() const
{
	int64_t first = 0;
	int64_t last = 0;
	size_t count = 0;
	for (size_t i = 0; i < packets_.size(); ++i) {
		const Packet &p = packets_[i];
		if (!IsVideoKey(p))
			continue;
		if (count == 0)
			first = p.dtsMs;
		last = p.dtsMs;
		++count;
	}
	if (count < 2 || last <= first)
		return 0.0;
	return static_cast<double>(last - first) / 1000.0 / static_cast<double>(count - 1);
}

double PacketHistory::BytesPerSecond() const
{
	const double span = SpanSeconds();
	if (span < 1.0)
		return 0.0;
	return static_cast<double>(bytes_) / span;
}

void PacketHistory::Clear()
{
	packets_.clear();
	store_.Clear();
	bytes_ = 0;
	frontSeq_ = nextSeq_;
	sawVideo_ = false;
	spliceFloorNs_ = INT64_MIN;
}

// -------------------------------------------------------------- scheduler --

SpliceInfo SinkScheduler::SetDelay(int64_t delayNs, int64_t nowNs, const PacketHistory &history)
{
	delayNs_ = std::max<int64_t>(0, delayNs);
	return TrySplice(nowNs, history);
}

SpliceInfo SinkScheduler::TrySplice(int64_t nowNs, const PacketHistory &history)
{
	SpliceInfo info;
	uint64_t seq = 0;
	if (!history.FindSplicePoint(nowNs - delayNs_, seq)) {
		needSplice_ = true;
		return info;
	}
	const Packet *key = history.At(seq);
	if (!key) {
		needSplice_ = true;
		return info;
	}

	info.spliced = true;
	info.shortHistory = key->arrivalNs > nowNs - delayNs_;
	if (haveVideoOut_ || haveAudioOut_)
		info.jumpSeconds = static_cast<double>(lastSentArrivalNs_ - key->arrivalNs) / 1e9;

	if (haveVideoOut_) {
		// The keyframe follows the last frame sent by one frame interval, in
		// decode order (dts) and in display order (pts): with B-frames the
		// frames sent last can show up to a few frames after their dts, and a
		// keyframe shown before them would play the stream backwards.
		offsetMs_ = std::max(lastVideoDtsOut_ + frameMs_ - key->dtsMs, maxVideoPtsOut_ + frameMs_ - key->ptsMs);
	} else if (haveAudioOut_)
		offsetMs_ = lastAnyDtsOut_ + frameMs_ - key->dtsMs;
	else
		offsetMs_ = -key->dtsMs;

	cursor_ = seq;
	started_ = true;
	needSplice_ = false;
	return info;
}

std::vector<OutPacket> SinkScheduler::Due(int64_t nowNs, const PacketHistory &history, size_t maxPackets,
					  const ReadyFn &ready)
{
	std::vector<OutPacket> out;
	if (!started_ && !needSplice_)
		return out;
	// The history was trimmed past this sink (it fell far behind): start again
	// from where the delay says it should be.
	if (needSplice_ || cursor_ < history.FrontSeq()) {
		if (!TrySplice(nowNs, history).spliced)
			return out;
	}

	while (cursor_ < history.EndSeq() && out.size() < maxPackets) {
		const Packet *p = history.At(cursor_);
		if (!p || p->arrivalNs + delayNs_ > nowNs)
			break;
		if (ready && !ready(*p))
			break;
		++cursor_;

		int64_t dts = p->dtsMs + offsetMs_;
		int64_t pts = p->ptsMs + offsetMs_;
		if (p->kind == MediaKind::Audio) {
			// Right after a splice the audio around the keyframe can land
			// before what was already sent: it is skipped, never reordered.
			if (haveAudioOut_ && dts <= lastAudioDtsOut_)
				continue;
			lastAudioDtsOut_ = dts;
			haveAudioOut_ = true;
		} else if (p->kind == MediaKind::Video) {
			if (haveVideoOut_ && dts <= lastVideoDtsOut_) {
				const int64_t shift = lastVideoDtsOut_ + 1 - dts;
				dts += shift;
				pts += shift;
			}
			if (haveVideoOut_) {
				const int64_t step = dts - lastVideoDtsOut_;
				if (step >= 1 && step <= 200)
					frameMs_ = (frameMs_ * 7 + step + 4) / 8;
			}
			lastVideoDtsOut_ = dts;
			maxVideoPtsOut_ = haveVideoOut_ ? std::max(maxVideoPtsOut_, pts) : pts;
			haveVideoOut_ = true;
		} else {
			if (dts < lastAnyDtsOut_)
				dts = lastAnyDtsOut_;
			if (pts < dts)
				pts = dts;
		}
		lastAnyDtsOut_ = std::max(lastAnyDtsOut_, dts);
		lastSentArrivalNs_ = p->arrivalNs;
		out.push_back(OutPacket{p, pts, dts});
	}
	return out;
}

int64_t SinkScheduler::NextDueNs(const PacketHistory &history) const
{
	if (needSplice_ || !started_)
		return -1;
	const Packet *p = history.At(cursor_);
	if (!p)
		return -1;
	return p->arrivalNs + delayNs_;
}

int64_t SinkScheduler::LagNs(int64_t nowNs, const PacketHistory &history) const
{
	const int64_t due = NextDueNs(history);
	if (due < 0 || due >= nowNs)
		return 0;
	return nowNs - due;
}

void SinkScheduler::ResetTimeline()
{
	haveVideoOut_ = false;
	haveAudioOut_ = false;
	lastVideoDtsOut_ = 0;
	maxVideoPtsOut_ = 0;
	lastAudioDtsOut_ = 0;
	lastAnyDtsOut_ = 0;
	offsetMs_ = 0;
}

int64_t RetentionNs(uint32_t delaySeconds, uint32_t minimumSeconds, double keyframeIntervalSeconds)
{
	const double gop = std::max(keyframeIntervalSeconds, 2.0);
	const double seconds = static_cast<double>(std::max(delaySeconds, minimumSeconds)) + 2.0 * gop + 1.0;
	return static_cast<int64_t>(seconds * 1e9);
}

} // namespace trigglow::relay
