*[Versión en español](STREAM_MODE-es.md)*

# Stream mode (v1.0.0)

Stream mode delays the stream OBS has **already encoded**, instead of the rendered frames. It is
the default from v1.0.0 wherever it can run (Windows); scene mode (buffer mode, `docs/SPEC.md`)
stays for the cases it can't.

## Why it costs almost nothing

Scene mode, per frame: render, read back from the GPU, compress into RAM (MJPEG or the GPU's
HEVC), decompress later, render again, and then OBS encodes it for the stream. Its RAM is the
buffer at a quality close to the source: ~190 MB to ~2.3 GB for 30-60 s (README table).

Stream mode keeps the packets OBS's own encoder produced for the stream and sends them later.
Nothing is decoded or encoded again; the CPU copies bytes. RAM is the stream's bitrate times the
seconds kept:

| Bitrate | 30 s | 60 s | 120 s (default kept) | 600 s |
|---|---|---|---|---|
| 3 Mbps | 11 MB | 22 MB | 45 MB | 225 MB |
| 6 Mbps | 22 MB | 45 MB | 90 MB | 450 MB |
| 8 Mbps | 30 MB | 60 MB | 120 MB | 600 MB |

Tools that relay the stream outside OBS work the same way. A figure of "0-15 MB" for such a tool can
only be its own baseline or a near-zero delay: a 6 Mbps stream held for 60 s is 45 MB, whatever
holds it.

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
   (`packet-delay-core.*`), stamping each packet with its arrival time.
5. Each sender's `SinkScheduler` sends packet *p* once `now >= arrival(p) + delay`, remuxed to FLV
   over RTMP/RTMPS (TLS per system, see Platforms), with the platform connection opened when the first packet
   is due (a stream that starts delayed starts that much later on the platform).

### Platforms

| | FFmpeg | TLS for RTMPS |
|---|---|---|
| Windows | BtbN's LGPL shared build (`avformat-63.dll` next to the plugin) | SChannel |
| Linux | the plugin's static LGPL build, linked in | the system's OpenSSL 3 (`libssl.so.3`, dynamic; a package dependency) |
| macOS | the plugin's static LGPL build, linked in, universal (arm64, x86_64) | Secure Transport (Security.framework) |

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

The history always reaches back `max(delay, kept)` seconds plus two keyframe intervals, so raising
the delay up to "Keep for instant raises" (default 120 s) needs no wait. Asking for more than the
history holds makes the sender hold until the oldest kept keyframe is old enough
(`SpliceInfo::shortHistory`).

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
  kept seconds (10-600), drain, the badge, where the stream goes (read only: OBS's Settings >
  Stream).
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
  - Long delays with "Keep for instant raises" at 600 s: 300, 120 and 600 s held to the frame
    (measured 300.0, 120.1, 600.0 s), back down to 30 s live. Raising beyond the history kept
    (30 -> 300 s 76 s into the stream, 120 -> 600 s at 436 s) holds: nothing at all (video or
    audio) reaches the platform for the missing seconds (232.6 s and 172.5 s here), then the
    stream goes on at the new delay. The local test platform kept the connection; a real platform
    may drop a publisher that sends nothing for minutes (not checked). Once the stream has run
    for the kept seconds, raises are instant replays.
  - RAM: the history is the stream's bitrate times the seconds kept (192 MB for 605 s at 2.66
    Mbps; it stops growing there). OBS's working set grew by the history plus ~15% (693 -> 906 MB)
    and went back to its idle size after the stream. The +275 MB when streaming starts is OBS's
    own (the same with stream mode off).
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
- Not run yet: a real platform (Twitch RTMP, Kick RTMPS, YouTube HEVC over enhanced RTMP) with
  its player watched, the dock UI by hand, and OBS reconnecting to the relay mid-stream (the relay
  listens once: if OBS's connection to it drops, the stream ends instead of resuming).
- The relay and the OBS integration build on Windows, Linux and macOS. On Linux the relay itself ran
  end to end with `tests/relay_harness.cpp` (ffmpeg standing in for OBS and the platform) against
  the plugin's static FFmpeg: RTMP and RTMPS (certificate and host name checked by OpenSSL), the
  delay 0 -> 6 -> 0 s live, and the drain after the stop. macOS and Linux haven't run in a real OBS
  yet.
