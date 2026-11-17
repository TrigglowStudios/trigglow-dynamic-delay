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

// v1.0.0 stream mode: the delay works on the stream OBS has ALREADY encoded,
// not on rendered frames. This file is its pure core -- no libobs, no FFmpeg,
// no Qt -- unit-tested in tests/packet_delay_core_tests.cpp.
//
// Why it costs almost nothing: buffer mode (video-delay-filter.*) reads every
// rendered frame back from the GPU, compresses it a second time to keep it in
// RAM, decompresses it again and lets OBS encode it a third time. Stream mode
// keeps the packets OBS's own encoder already produced for the stream: the
// memory is the stream's bitrate times the seconds kept (6 Mbps x 60 s = 45 MB)
// and the CPU is copying bytes. See docs/STREAM_MODE.md.
//
// Model:
// - PacketHistory: every packet the stream produced in the last `retention`
//   seconds, in arrival order, each stamped with its arrival time. It always
//   starts at a video keyframe, so a sink can splice there. The packets' bytes
//   live back to back in its PayloadStore (payload-store.hpp); each Packet is
//   only the metadata the scheduler needs.
// - SinkScheduler: one per destination. It sends packet p once
//   now >= p.arrival + delay. Changing the delay SPLICES: the sink jumps to the
//   newest video keyframe old enough for the new delay (back in time to add
//   delay -- viewers see the last seconds again, never anything newer; forward
//   to remove it) and rewrites timestamps so what leaves stays continuous.
//   The destination never sees a cut or a reconnect.

#include "payload-store.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace trigglow::relay {

enum class MediaKind : uint8_t { Video, Audio, Other };

// One encoded packet's metadata. Timestamps are in milliseconds (FLV's own
// timebase, which is what RTMP carries). Its bytes are a record in the
// history's PayloadStore, opaque to the core (the relay's serialized FFmpeg
// packet; tests usually store none).
struct Packet {
	int64_t ptsMs = 0;
	int64_t dtsMs = 0;
	int64_t arrivalNs = 0;     // Monotonic clock, when the packet reached the relay.
	uint64_t recordOffset = 0; // Set by PacketHistory::Push().
	uint32_t recordBytes = 0;  // Set by PacketHistory::Push().
	uint32_t bytes = 0;        // Payload size, for the bitrate.
	MediaKind kind = MediaKind::Other;
	uint8_t stream = 0;
	bool keyframe = false;
};

class PacketHistory {
public:
	// Appends `packet`, with `parts` stored back to back as its record, and
	// returns its seq (seqs only grow; At() finds a packet by it).
	uint64_t Push(Packet packet, const PayloadStore::Span *parts = nullptr, size_t partCount = 0);

	// Appends the packet's record to `out`. False when it isn't available
	// (or, with the spill, not back in RAM yet).
	bool CopyRecord(const Packet &packet, std::vector<uint8_t> &out) const
	{
		return store_.CopyOut(packet.recordOffset, packet.recordBytes, out);
	}
	bool RecordInRam(const Packet &packet) const { return store_.InRam(packet.recordOffset, packet.recordBytes); }
	const PayloadStore &Store() const { return store_; }
	PayloadStore &Store() { return store_; }

	// Store offset of the first packet that arrived at or after `atNs` (the
	// store's end when none did).
	uint64_t OffsetAtArrival(int64_t atNs) const;
	// The bytes of packet `seq` and of every packet that arrived up to
	// `aheadNs` after it: what a sender at `seq` sends next.
	PayloadStore::Range AheadRange(uint64_t seq, int64_t aheadNs) const;
	// Drops the packets whose bytes the store lost (PayloadStore::LostBefore),
	// up to the next keyframe. Senders behind the new front re-splice.
	void DropLost();

	// Drops whole GOPs from the front while the NEXT video keyframe is still
	// older than now - retention, and never past `minKeepSeq` (the oldest
	// packet a sink still needs). Audio-only streams trim packet by packet.
	void Trim(int64_t nowNs, int64_t retentionNs, uint64_t minKeepSeq);

	// The newest video keyframe that arrived at or before `atNs`. Falls back
	// to the oldest keyframe kept when none is that old (the history is
	// shorter than the delay asked for); false only with no keyframe at all.
	// With no video at all, the packet at or before `atNs` (or the oldest).
	bool FindSplicePoint(int64_t atNs, uint64_t &seqOut) const;

	// Dump: nothing that arrived before `floorNs` may be spliced to again, so
	// what viewers hadn't seen yet when the streamer pressed Dump is never
	// sent. FindSplicePoint() then only returns keyframes that arrived at or
	// after it (none yet: false, and the sinks wait for the next one).
	void SetSpliceFloor(int64_t floorNs) { spliceFloorNs_ = floorNs; }
	int64_t SpliceFloor() const { return spliceFloorNs_; }

	const Packet *At(uint64_t seq) const;
	uint64_t FrontSeq() const { return frontSeq_; }
	uint64_t EndSeq() const { return frontSeq_ + packets_.size(); }
	bool Empty() const { return packets_.empty(); }
	// Payload bytes of the packets kept.
	size_t Bytes() const { return bytes_; }
	// RAM held for the history: records plus the packets' metadata.
	size_t RamBytes() const { return store_.RamBytes() + packets_.CapacityBytes(); }
	bool HasVideo() const { return sawVideo_; }

	// Seconds the history covers (newest arrival - oldest arrival).
	double SpanSeconds() const;
	// Average distance between video keyframes over the history, or 0 when
	// fewer than two were seen. The delay can only move in steps of this.
	double KeyframeIntervalSeconds() const;
	// Bytes per second over the history (0 until it spans 1 s).
	double BytesPerSecond() const;

	void Clear();

private:
	ChunkedQueue<Packet> packets_;
	PayloadStore store_;
	uint64_t frontSeq_ = 0;
	uint64_t nextSeq_ = 0;
	size_t bytes_ = 0;
	bool sawVideo_ = false;
	int64_t spliceFloorNs_ = INT64_MIN;
};

// A packet ready to leave, with its timestamps rewritten for the destination.
struct OutPacket {
	const Packet *packet = nullptr;
	int64_t ptsMs = 0;
	int64_t dtsMs = 0;
};

struct SpliceInfo {
	bool spliced = false;
	// Seconds of already-sent stream the destination sees again (adding
	// delay) or that are skipped (negative: removing it). 0 when unchanged.
	double jumpSeconds = 0.0;
	// True when the history is shorter than the delay asked for: the sink waits
	// (the picture holds) until the oldest kept keyframe is old enough.
	bool shortHistory = false;
};

class SinkScheduler {
public:
	// Starts (or restarts, after a reconnect) at the splice point for
	// `delayNs`. The next packet sent continues the timeline that was being
	// sent before, so a reconnect or a delay change never makes timestamps go
	// backwards.
	SpliceInfo SetDelay(int64_t delayNs, int64_t nowNs, const PacketHistory &history);

	// Packets due at `nowNs`, in order, with rewritten timestamps. Audio that
	// would go back in time right after a splice is skipped. `ready` (when
	// set) says whether a packet's bytes can be sent now: the first that
	// can't stops the batch, and is due again next time.
	using ReadyFn = std::function<bool(const Packet &)>;
	std::vector<OutPacket> Due(int64_t nowNs, const PacketHistory &history, size_t maxPackets = 256,
				   const ReadyFn &ready = {});

	// Monotonic time the next packet is due, or -1 when the sink is caught up
	// with the history.
	int64_t NextDueNs(const PacketHistory &history) const;

	// How late the next due packet is (0 when on time). The relay re-splices
	// when the destination can't keep up, instead of letting the delay grow.
	int64_t LagNs(int64_t nowNs, const PacketHistory &history) const;

	// Forgets the timeline: the next SetDelay() starts it at 0 (a new
	// destination connection that wants a fresh one).
	void ResetTimeline();

	bool Started() const { return started_; }
	uint64_t CursorSeq() const { return cursor_; }
	int64_t DelayNs() const { return delayNs_; }
	bool CaughtUp(const PacketHistory &history) const { return cursor_ >= history.EndSeq(); }

private:
	SpliceInfo TrySplice(int64_t nowNs, const PacketHistory &history);

	bool started_ = false;
	// SetDelay() came before the history had a splice point (no keyframe yet):
	// Due() splices as soon as one arrives.
	bool needSplice_ = false;
	uint64_t cursor_ = 0;
	int64_t delayNs_ = 0;
	int64_t offsetMs_ = 0;
	bool haveTimeline_ = false;
	int64_t lastVideoDtsOut_ = 0;
	int64_t maxVideoPtsOut_ = 0; // Newest display time sent (B-frames: ahead of dts).
	int64_t lastAudioDtsOut_ = 0;
	int64_t lastAnyDtsOut_ = 0;
	bool haveVideoOut_ = false;
	bool haveAudioOut_ = false;
	int64_t frameMs_ = 33; // Video frame spacing, learnt from what is sent.
	int64_t lastSentArrivalNs_ = 0;
};

// Clamp for the user-facing delay: 0 (live) to 600 s.
constexpr uint32_t kMaxDelaySeconds = 600;
inline uint32_t ClampDelaySeconds(int64_t seconds)
{
	if (seconds < 0)
		return 0;
	if (seconds > kMaxDelaySeconds)
		return kMaxDelaySeconds;
	return static_cast<uint32_t>(seconds);
}

// How long the history must reach back: the delay asked for, at least
// `minimumSeconds` (so the delay can be raised live without waiting), plus
// two keyframe intervals of slack for the splice.
int64_t RetentionNs(uint32_t delaySeconds, uint32_t minimumSeconds, double keyframeIntervalSeconds);

// RetentionNs's `minimumSeconds`: how far a live raise is instant. With the
// spill (payload-store.hpp) the older seconds cost disk, not RAM, so it is the
// most a delay can be: every raise is instant (once the stream has run that
// long). In RAM only, `ramSeconds`: the furthest the streamer can jump in one
// step (the controller's highest preset or delay-on scene rule).
inline uint32_t KeepSeconds(bool onDisk, uint32_t ramSeconds)
{
	return onDisk ? kMaxDelaySeconds : ClampDelaySeconds(ramSeconds);
}

// RAM the history needs for `seconds` at `bitsPerSecond`, in bytes.
inline uint64_t EstimateBytes(double bitsPerSecond, double seconds)
{
	if (bitsPerSecond <= 0 || seconds <= 0)
		return 0;
	return static_cast<uint64_t>(bitsPerSecond / 8.0 * seconds);
}

} // namespace trigglow::relay
