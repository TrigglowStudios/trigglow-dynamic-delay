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

// v1.0.0 stream mode: how the relay publishes to an rtmp:// or rtmps://
// destination (stream-relay.cpp). The packets still go through FFmpeg's FLV
// muxer (codec headers, enhanced RTMP), and the connection is still FFmpeg's
// tcp:// or tls:// protocol (SChannel on Windows: SNI, certificate checks);
// only the RTMP layer in between is the relay's own (rtmp-wire.hpp), so it
// sends 4096-byte chunks to every server and a batch of packets in a few
// large writes. docs/STREAM_MODE.md, "CPU".
//
// One thread uses it (the destination's sender): the server's messages (ping
// requests, acknowledgements, an error status) are read between writes, never
// at the same time as one, with the connection switched to non-blocking for
// that read only -- what FFmpeg's own RTMP client does after every write.
//
// Open() says when FFmpeg's RTMP client should be used instead: the FFmpeg
// in use doesn't look like the one this was written against (Unsupported),
// or the server dropped the session in a way that may be this code's fault
// (ProtocolProblem).

#include <cstdint>
#include <memory>
#include <string>

struct AVIOContext;
struct AVIOInterruptCB;

namespace trigglow::relay {

class RtmpPublisher {
public:
	enum class OpenResult {
		Ok,
		Failed,          // Network, TLS, or the server refused the stream.
		ProtocolProblem, // Closed or silent during the setup, or unreadable.
		Unsupported,     // Use FFmpeg's RTMP client for this one.
	};

	RtmpPublisher();
	~RtmpPublisher();
	RtmpPublisher(const RtmpPublisher &) = delete;
	RtmpPublisher &operator=(const RtmpPublisher &) = delete;

	// rtmp:// and rtmps:// URLs with a host.
	static bool Handles(const std::string &server);

	// Connects, handshakes and publishes `key` on `server` (the app is the
	// URL's path). `interrupt` must outlive the publisher. `error` is short
	// and never contains the key.
	OpenResult Open(const std::string &server, const std::string &key, const AVIOInterruptCB *interrupt,
			std::string &error);

	// Where FFmpeg's FLV muxer writes (AVFormatContext::pb, with
	// AVFMT_FLAG_CUSTOM_IO). Owned by the publisher.
	AVIOContext *MuxerIo();

	// Sends everything the muxer has written, then (at most every 100 ms)
	// reads and answers the server's messages. False when the connection or
	// the server ended the stream.
	bool Flush(std::string &error);
	// The sender is idle and the server's messages haven't been read for a
	// while (pings must be answered).
	bool ServiceDue() const;

	// Ends the stream (FCUnpublish, deleteStream when `unpublish`) and closes
	// the connection. The muxer must be done with MuxerIo().
	void Close(bool unpublish);

	// Bytes written to the connection (before TLS).
	uint64_t BytesSent() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace trigglow::relay
