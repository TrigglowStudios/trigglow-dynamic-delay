*[Versión en español](STREAM_MODE-es.md)*

# Stream mode (v1.0.0)

Stream mode delays the stream OBS has **already encoded**, instead of the rendered frames. It is
the default from v1.0.0 wherever it can run (Windows, and macOS and Linux as alpha); scene mode
(buffer mode, `docs/SPEC.md`) stays for the cases it can't.

## Why it costs almost nothing

Scene mode, per frame: render, read back from the GPU, compress into RAM (MJPEG or the GPU's
HEVC), decompress later, render again, and then OBS encodes it for the stream. Its RAM is the
buffer at a quality close to the source: ~190 MB to ~2.3 GB for 30-60 s (README table).

Stream mode keeps the packets OBS's own encoder produced for the stream and sends them later.
Nothing is decoded or encoded again; the CPU copies bytes. The history is the stream's bitrate times
the seconds kept (45 MB per minute at 6 Mbps), and most of it waits in an encrypted temp file on
disk, so the RAM it takes stays about the same at any delay (see Memory and the disk). Its CPU is
about 1-1.5 s per minute of stream with Twitch, Kick or Facebook, and about 10 more for each YouTube
destination (see CPU).

## Memory and the disk

- **Contiguous storage.** The packets' bytes go back to back into 1 MiB blocks
  (`PayloadStore`, `payload-store.*`), one record per packet: its flags, side data and payload,
  exactly what FFmpeg's demuxer returned, so the platform gets the same bytes. The scheduler's
  metadata is 48 bytes per packet. Before, every packet was its own FFmpeg allocation, which cost
  10-13% on top of the history (below: +490 MB of RAM for 446 MB of history, +106 MB for 93 MB).
- **The spill.** Only the newest seconds (4 s plus two keyframe intervals: live and short delays
  never touch the disk, and lowering the delay splices into RAM) and what each destination sends
  next (3 s ahead of it) stay in RAM. Older full blocks go to a temp file and their RAM is freed. A
  block a destination is about to send is read back on the relay's own I/O thread before it is due,
  never under the lock the senders take; a sender whose bytes aren't back yet waits instead of
  sending.
- **Encrypted.** AES-256 in counter mode with a random key and nonce made for each stream and kept
  only in OBS's memory; the counter comes from the block's number, which is never reused. The AES
  is the system's: CNG (bcrypt) on Windows, CommonCrypto on macOS, the OpenSSL the Linux build
  already links for RTMPS. No new dependency.
- **Where, and for how long.** The system's temp folder (on Linux `$TMPDIR`, `/var/tmp` or `/tmp`,
  the first that isn't a RAM-backed tmpfs), a file named `trigglow-dd-spill-*.tmp`. It is deleted when the stream ends, and also
  when OBS crashes: delete-on-close on Windows, unlinked as soon as it is created on Linux and macOS.
  The next stream deletes any a power loss left. Freed space is reused and the file is truncated as
  the history shrinks.
- **Disk use.** The bitrate times 600 s while streaming in stream mode (about 450 MB at 6 Mbps, 600
  MB at 8 Mbps), written and, for delays longer than the RAM window, read back at the stream's
  bitrate. The spill always leaves 1 GiB free on that disk.
- **When the disk can't be used** (no room, a write or read fails, a block takes more than 2 s, or
  48 blocks wait to be written), the spill stops for that stream: what is on disk is read back and
  the history stays in RAM, as before the spill, at the bitrate times the seconds kept (see Changing
  the delay live). It is logged and the stream goes on. Only a block that can't be read back at all
  is dropped, with its packets; the destinations re-splice at the next keyframe.

Measured with `tests/relay_harness.cpp` (the relay in its own process, ffmpeg standing in for OBS
and the platform; private bytes over the process before the stream, 18.9-21.1 MB) with a 6.2 Mbps
x264 stream (1280x720 30 fps, B-frames, 2 s keyframes, AAC 160 kbps), Windows 11, 2026-11-11:

| Delay | History kept (before / now) | RAM before | Contiguous blocks only | RAM now | of it, the history | On disk now |
|---|---|---|---|---|---|---|
| 10 s | 127 s, 93 MB / 607 s, 446 MB | +106 MB | +96 MB | +13 MB | 11 MB | 440 MB |
| 120 s | 127 s, 93 MB / 607 s, 446 MB | +103 MB | +96 MB | +15 MB | 13 MB | 440 MB |
| 600 s | 607 s, 446 MB / 607 s, 446 MB | +490 MB | +453 MB | +13 MB | 12 MB | 440 MB |
| 600 s, RAM only (no spill) | 607 s, 446 MB | | | +454 MB | 449 MB | 0 |

"+N MB" is how much the process grew (private bytes) over itself before the stream. "Before" is the
relay without these changes, with "Keep for instant raises" at its default (120 s); "Contiguous
blocks only" is the first change without the spill. Within OBS, the history is the only part of
stream mode that grows with the delay.

Tools that relay the stream outside OBS hold the same bytes: a 6 Mbps stream held for 60 s is 45 MB
wherever it is. Stream mode keeps it on disk, encrypted, and in RAM only what is about to be sent.

## CPU

The relay's CPU is almost all the operating system moving RTMP's bytes through sockets, and how
much depends on the size of the RTMP chunks on each side, which the relay doesn't choose:

- **From OBS.** OBS sends 4096-byte chunks, one write per chunk (`obs-outputs`: `rtmp-stream.c`
  sets `m_outChunkSize = 4096`; librtmp writes each chunk with its header). At 6 Mbps the relay's
  FFmpeg listener reads about 190 chunks a second.
- **To the platform.** FFmpeg's RTMP client (FFmpeg 9.0.2, `rtmppkt.c`) writes every chunk with two
  `send()` calls (the chunk, then the next chunk's one-byte header), each after a `select()`. It
  starts with 128-byte chunks and only switches to the size the server announces. Twitch, Kick
  (Amazon IVS) and Facebook announce 4096 bytes when the relay connects; YouTube (RTMP and RTMPS)
  announces nothing, so FFmpeg stays at 128 bytes: about 6000 chunks and 12,000 tiny sends a second
  at 6 Mbps. FFmpeg has no option for it.

Measured with `tests/relay_harness.cpp` (the relay in its own process; CPU of that process only)
and the peers in `tools/relay-cpu/` (`rtmp_peers.py` publishes like OBS and stands in for the
platform), a 6.2 Mbps x264 stream (1920x1080 30 fps, CBR, `-g 60 -bf 2`, AAC 160 kbps), 300 s,
the delay set when OBS's stream reaches the relay. Windows 11, Ryzen 7 7700X, 2026-11-11. CPU
seconds per minute of stream while OBS and the destination are both sending (exact CPU cycles in
brackets, millions per second):

| Delay | Before: ffmpeg as OBS and as the platform (the benchmark's setup) | OBS, platform announcing 4096 bytes (Twitch, Kick, Facebook) | OBS, platform announcing nothing (YouTube) |
|---|---|---|---|
| 0 s | 12.4 s | 0.9 s (88) | 11.1 s (772) |
| 10 s | 12.9 s | 1.4 s (91) | 10.2 s (769) |
| 120 s | 11.9 s | 1.6 s (97) | 10.4 s (778) |
| 120 s, OBS only (before the delay has passed) | 3.7 s | 0.7 s (41) | 0.7 s (39) |
| 120 s, sending only (draining after the stop) | 8.5 s | 0.9 s (73) | 9.6 s (693) |

The relay's code is the same in all three columns. The 14-20 CPU s per minute in
`docs/BENCHMARK-buffer-storage.md` came from ffmpeg standing in for OBS and for the platform: ffmpeg's
RTMP server announces 128 bytes and ffmpeg's client copies it, so both of the relay's sockets carried
128-byte chunks (the ingest thread woke about 10,000 times a second). With OBS and Twitch, Kick or
Facebook, stream mode takes about 1-1.5 CPU s per minute of stream (2-3% of one core). Each YouTube
destination adds about 10 (a sixth of one core).

What was checked and isn't the cost:

- **The relay's own threads.** Sampled per thread (stack samples, `QueryThreadCycleTime`, context
  switches, 60 s at delay 10 with OBS-like peers and a 4096-byte platform): the sender 70 million
  cycles a second, 85% of it kernel time in `send()`/`select()`; the listener 46 million, a third
  of it kernel time; the spill's I/O thread 5 million. All user-mode time together (FFmpeg's
  demuxing and muxing included) is about 0.4 CPU s per minute. With a 128-byte platform the sender
  alone is about 700 million cycles a second, 96% kernel time.
- **Waits.** No busy waits: MSVC's `condition_variable::wait_for` rounds up to whole milliseconds, a
  sender sleeps until its next packet is due (100-200 wake-ups a second), and a wake-up costs about
  50,000 cycles here. The spill's I/O thread wakes 20 times a second (4 million cycles a second).
- **Per packet:** one FFmpeg packet allocation and copy per packet sent (about 80 a second), FLV
  muxing, the status once a second, no logging. None shows up in the samples.
- **`TCP_NODELAY` off** on the destination's socket (Nagle merges FFmpeg's small writes): 9.8 -> 8.3
  CPU s per minute with 128-byte chunks, no change with 4096. Not worth the extra latency, so it
  stays on.

Not done: sending to YouTube at OBS's cost needs the relay to write RTMP chunks itself (announce
4096 bytes, one write per chunk or per batch) instead of FFmpeg's client; FFmpeg has no option or
public API for it. It must then be checked against the real platforms. Also not measured: RTMPS to a
128-byte server, where each of those small writes is also its own TLS record; YouTube with a valid
stream key (the probe only connected, and published with an invalid key: no chunk size either way).

## How it works

1. On `OBS_FRONTEND_EVENT_STREAMING_STARTING` (after OBS created the stream output, before it
   starts) `StreamDelayController` checks the stream is eligible: OBS's `rtmp_output`, protocol
   RTMP or RTMPS, not Enhanced Broadcasting (multitrack). It reads the server and key with
   `obs_service_get_connect_info()` (the resolved ingest, "auto" included).
2. It starts `StreamRelay` (FFmpeg avformat): a listener on `127.0.0.1:<free port>` and one
   sender to the streamer's own service. Dynamic Delay doesn't multistream: `StreamRelay::Start()`
   takes a list of destinations, but the controller only ever passes that one.
3. It points **that output** at the relay with a private `rtmp_custom` service. The streamer's saved
   service is untouched; OBS sets it on the output again at every start.
4. OBS publishes to the relay; the relay demuxes the FLV (no decoding) into `PacketHistory`
   (`packet-delay-core.*`), stamping each packet with its arrival time. Its bytes go to the
   history's `PayloadStore` and, once older than a few seconds, to the encrypted spill file.
5. Each sender's `SinkScheduler` sends packet *p* once `now >= arrival(p) + delay`, remuxed to FLV
   over RTMP/RTMPS (TLS per system, see Platforms), with the platform connection opened when the first packet
   is due (a stream that starts delayed starts that much later on the platform).

### Platforms

| | FFmpeg | TLS for RTMPS | AES for the spill |
|---|---|---|---|
| Windows | BtbN's LGPL shared build (`avformat-63.dll` next to the plugin) | SChannel | CNG (bcrypt) |
| Linux | the plugin's static LGPL build, linked in | the system's OpenSSL 3 (`libssl.so.3`, dynamic; a package dependency) | the same OpenSSL (`libcrypto.so.3`) |
| macOS | the plugin's static LGPL build, linked in, universal (arm64, x86_64) | Secure Transport (Security.framework) | CommonCrypto |

The Linux and macOS FFmpeg comes from `.github/scripts/build-ffmpeg.sh` at buildspec.json's
`ffmpeg-source` pin (signed tag, pinned commit), with only what the relay needs: FLV demuxer and
muxer, the RTMP, RTMPS, TCP and TLS protocols, and the H.264, HEVC, AV1 and AAC parsers. Its
symbols are hidden, so they never meet the avformat OBS loads; CI checks it (`package.sh` with `nm
-D` on Linux, the macOS job's "Check Plugin Binary" with `nm`). The core-only build has no relay.

### Changing the delay live

The sender **splices**: it jumps to the newest video keyframe that is at least *delay* old and
rewrites timestamps so the next frame follows the last one sent by one frame interval. Adding delay
jumps back: viewers see the last seconds again, never anything newer. Removing it jumps forward.
The platform sees one continuous stream: no reconnect, no new header. The step is the keyframe
interval (2 s recommended); the dock warns when it is on auto or above 4 s (see Settings checks).

With the spill the history always reaches back 600 s (the most a delay can be) plus two keyframe
intervals plus 1 s: it costs disk, not RAM, and every live raise is instant once the stream has run
that long. In RAM only (the disk couldn't be used) it reaches back the delay or the highest preset
or delay-on scene rule, whichever is higher, plus the same slack (`KeepSeconds()` and
`RetentionNs()` in `packet-delay-core.hpp`). The dock's "Instant raises" line says which applies.
Asking for more than the history holds (at the start of a stream, or beyond the presets in RAM only)
makes the sender hold until the oldest kept keyframe is old enough (`SpliceInfo::shortHistory`).

### Network problems

- If the connection to the platform can't connect or send, the sender retries with back-off (2, 4,
  8, 15 s) and a fresh timeline, at the delay.
- If the sender falls more than 4 s behind its delay (slow upload), it jumps to the delay again at
  the next keyframe, instead of letting the delay grow (`resyncs` in the status).
- OBS's own congestion handling only sees the loopback, so OBS's dropped-frames counter stays at 0
  in stream mode; the dock shows the connection's state and rate instead.

### Stopping

When OBS stops, the relay keeps sending what viewers haven't seen yet (the last *delay* seconds),
unless "When I stop streaming, send the seconds viewers haven't seen" is off; the dock offers "Stop
sending now". A new stream, or closing OBS, cuts it.

### Dump

"Dump what viewers haven't seen" (dock, hotkey, obs-websocket `DumpDelay`) sets a splice floor at
now on the history (`PacketHistory::SetSpliceFloor`) and the delay to 0, under the lock the sender
takes before `Due()`. The sender re-splices to the first keyframe that arrives after the floor
(within one keyframe interval), so the seconds still waiting in the delay are never sent. The
controller raises the delay again after its seconds plus 3 s; the splice can't go below the floor,
so it only rewinds into what came after the dump. Checked with the harness: dump at 25 s with a
10 s delay, live within ~2 s, delay back, timestamps continuous, output decodes clean.

### Scene rules and presets

Rules map a scene to "delay on (with its seconds)" or "delay off", applied on
`OBS_FRONTEND_EVENT_SCENE_CHANGED` and when a stream starts. Three presets (default 10/30/60 s) set
the seconds and turn the delay on, each with a hotkey and `ApplyPreset {index}` over obs-websocket.

## What stays live

Recording, the Replay Buffer and the virtual camera read OBS's program, which stream mode never
touches: they keep the live picture. Every scene is delayed, so switching scenes while delayed is
safe (scene mode only delays the scene picked in the dock).

## The delay badge

While the delay is on and the stream goes through the relay, the "DELAY N s" badge (the same PNGs
as scene mode, up to 600 s) is drawn on libobs output channel 61, over OBS's program after the
scene. Viewers get it with the delayed picture. Because it is on the program, the recording (and
anything else that takes the program) shows it too while the delay is on. On/off ("Show the delay
badge to viewers") and the corner are in the dock and saved.

## Settings checks

When a stream starts, the controller looks at OBS's settings and the dock lists what makes stream
mode worse (`StreamDelayController::CheckObsSettings`):

- the stream encoder's keyframe interval on auto (0, often 4-10 s) or above 4 s: the delay moves
  in steps of it;
- OBS's own Stream Delay (Settings > Advanced) on: its fixed delay adds up with this one.

## Controls

- Dock: on/off, back to live, seconds (1-600, +/-5), dump, three presets, delay by scene,
  how far a raise is instant (read only; it replaced the "Keep for instant raises" setting, whose
  saved `keep_seconds` is still read and written back unchanged), drain, the badge, where the
  stream goes (read only: OBS's Settings > Stream). The stats show the RAM and the disk the history
  takes.
- Hotkeys (OBS Settings > Hotkeys): Toggle, Enable, Disable (names unchanged), Back to live,
  Delay +5 s, Delay -5 s, Dump, Preset 1-3. They go through `DelayRouter`: stream mode when it
  handles the controls, scene mode otherwise (there too the seconds change live, 1-600, and a preset also turns the delay
  on; Dump does nothing).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); event `StatusChanged`. For the Trigglow Stream Deck plugin; details
  in `docs/STREAM_DECK.md`.

The free-account gate applies to Enable as in scene mode.

## Not covered yet

- Enhanced Broadcasting (multitrack video), SRT, RIST and WHIP services: scene mode.
- Delay by scene only in stream mode: scene mode keeps delaying the one live scene chosen in the
  dock.

## Verification status

- `packet-delay-core` is unit-tested (`tests/packet_delay_core_tests.cpp`): live passthrough,
  rewinding on enable with the delay held and monotonic timestamps, skipping forward on disable,
  short history, splicing before the first keyframe, trimming, audio only, dump, and splices in a
  B-frame stream (x264's default) keeping the display order.
- End to end in a real OBS 33.0.1 on Windows 11 (portable copy, x264 CBR 2500 kbps, 1280x720 30
  fps, 2 s keyframes), driven over obs-websocket 5, with a local FFmpeg RTMP listener as the
  platform (2026-11-11). A 12-bit Gray-code clock drawn into the scene measured the delay viewers
  get from the platform's arrival times; every output was checked with ffprobe/ffmpeg (monotonic
  timestamps, no gaps beyond ~150 ms at a splice, decodes clean):
  - Advanced and Simple output modes; OBS's own Stream Delay on top (the delays add up).
  - Live passthrough costs nothing measurable: 1.4 s from OBS's canvas to the platform, the same as
    without the relay. Delay on, raised, lowered, off, presets, dump (live within a keyframe, the
    delay back after its seconds + 3 s), the scene rule, every vendor request and every hotkey
    (TriggerHotkeyByName): the measured delay is the one asked for plus those 1.4 s.
  - Stop with the delay on sends the last seconds and finishes; a new stream while draining cuts it;
    closing OBS mid-stream or mid-drain exits in ~0.3 s.
  - Scene switches while delayed (12, down to 0.3 s apart, delay 10 s): every switch reached
    viewers 11.4-11.6 s later, the delay stayed 11.37-11.53 s on every frame, no frame showed a
    scene that wasn't on at its content time, nothing newer than the delay got out, no cut.
  - 19 live changes up and down in one stream (hotkeys +/-5, the three presets, `SetDelay`,
    `AdjustDelay`, off and on, 3-40 s): each measured within 0.04 s of the delay asked for, one
    connection to the platform throughout, timestamps continuous.
  - Long delays with "Keep for instant raises" at 600 s (before it was replaced): 300, 120 and 600 s held to the frame
    (measured 300.0, 120.1, 600.0 s), back down to 30 s live. Raising beyond the history kept
    (30 -> 300 s 76 s into the stream, 120 -> 600 s at 436 s) holds: nothing at all (video or
    audio) reaches the platform for the missing seconds (232.6 s and 172.5 s here), then the
    stream goes on at the new delay. The local test platform kept the connection; a real platform
    may drop a publisher that sends nothing for minutes (not checked). Once the stream has run
    for the kept seconds, raises are instant replays.
  - RAM, before the spill and the contiguous storage: the history is the stream's bitrate times the
    seconds kept (192 MB for 605 s at 2.66 Mbps; it stops growing there). OBS's working set grew by
    the history plus ~15% (693 -> 906 MB) and went back to its idle size after the stream. The +275
    MB when streaming starts is OBS's own (the same with stream mode off). Not measured again in a
    real OBS since (see the table in Memory and the disk for the relay on its own).
  - 12 start/stop cycles, half with every core busy: OBS always reached the relay (listener ready
    in 5-50 ms).
  - The free-account gate refuses Enable, presets and Toggle when signed out (a test-only build
    switch, `TRIGGLOW_TEST_SKIP_ACCOUNT_GATE`, refused in CI, skipped it for the rest).
  - The badge is drawn while the delay is on. It is part of the delayed picture, so viewers see the
    value that was on when the frame was rendered: for the first *delay* seconds after a change
    the old value (or none, right after Enable).
- Fixed by these runs: in Advanced output mode OBS set its own service on the output after
  `STREAMING_STARTING`, so the stream bypassed the relay (the frontend's service is now swapped
  for the relay's during the start call and put back right after); B-frame splices played
  backwards; a stop before OBS reached the relay, or a dead destination, left it draining forever;
  the 150 ms wait for the listener is now a check that it listens (or the stream goes out without
  the relay; on Linux and macOS it is still a fixed 150 ms wait).
- Contiguous storage, the spill and its retention (2026-11-11), Windows 11, with
  `tests/relay_harness.cpp` and a 6.2 Mbps x264 stream with B-frames (`-bf 2 -g 60`, CBR, AAC):
  - Unit tests: `payload_store_tests` (blocks and trimming; the spill's state machine with a
    scripted disk: round trip, trim and truncation, a failed write, a failed read, a slow disk, a
    backlog, a full file), `spill_file_tests` (the NIST SP 800-38A CTR-AES256 vector, round trip,
    nothing of the plaintext on disk, a different keystream per block and per file,
    delete-on-close, truncation, leftover files), retention and the spill's hooks in
    `packet_delay_core_tests`. The same three also build without warnings and pass on Linux (GCC
    13, OpenSSL 3, an Ubuntu 24.04 container).
  - Live changes `4:3,10:8,18:2,24:0` and a raise to 300 s read back from disk
    (`5:10,330:300,360:20,375:0`): every packet sent is byte-identical to a packet of the source
    (sha256, size and flags, `ffprobe -show_packets -show_data_hash`), the splices land on the same
    keyframes as the relay before the change, video DTS strictly increasing, no gap over 200 ms,
    decodes clean. Two real-time runs never match packet for packet, not even two runs of the same
    build (the audio skipped around a splice and the frame the splice follows depend on what was
    due at that instant), so outputs were checked against the source instead.
  - A spill capped at 70 MB fell back to RAM when full (logged), read the spilled blocks back and
    went on; the output stayed byte-identical with no gap. Killing the process mid-stream deleted
    its spill file at once.
- CPU (2026-11-11), the setups in CPU above, 10 relays at a time: the live changes
  `4:3,10:8,18:2,24:0` and a raise from 10 to 300 s at t=200 (190 s replayed from the spill), with
  ffmpeg peers and with OBS-like peers, and the steady runs at 0, 10 and 120 s against both kinds of
  platform: every packet sent is byte-identical to a packet of the source (sha256), video DTS
  strictly increasing, no DTS step over 200 ms (largest: 34 ms video, 96 ms audio), decodes clean.
  The public ingests' chunk sizes were read with an anonymous RTMP connect (no stream key, nothing
  published).
- Not run yet: a real platform (Twitch RTMP, Kick RTMPS, YouTube HEVC over enhanced RTMP) with
  its player watched, the dock UI by hand (also the new "Instant raises" line), the spill inside a
  real OBS, the spill at runtime on macOS (not built here) and Linux (unit tests only), a slow
  (USB, HDD) or nearly full disk in practice, and OBS reconnecting to the relay mid-stream (the
  relay listens once: if OBS's connection to it drops, the stream ends instead of resuming).
- The relay and the OBS integration build on Windows, Linux and macOS. On Linux the relay itself ran
  end to end with `tests/relay_harness.cpp` (ffmpeg standing in for OBS and the platform) against
  the plugin's static FFmpeg: RTMP and RTMPS (certificate and host name checked by OpenSSL), the
  delay 0 -> 6 -> 0 s live, and the drain after the stop. macOS and Linux haven't run in a real OBS
  yet.
