# Stream mode (v1.0.0)

Stream mode delays the stream OBS has **already encoded**, instead of the rendered frames. It is
the default from v1.0.0 wherever it can run; scene mode (buffer mode, `docs/SPEC.md`) stays for the
cases it can't.

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
2. It starts `StreamRelay` (FFmpeg avformat, Windows): a listener on `127.0.0.1:<free port>` and one
   sender per destination: the streamer's service first, then the extra destinations from the dock.
3. It points **that output** at the relay with a private `rtmp_custom` service. The streamer's saved
   service is untouched; OBS sets it on the output again at every start.
4. OBS publishes to the relay; the relay demuxes the FLV (no decoding) into `PacketHistory`
   (`packet-delay-core.*`), stamping each packet with its arrival time.
5. Each sender's `SinkScheduler` sends packet *p* once `now >= arrival(p) + delay`, remuxed to FLV
   over RTMP/RTMPS (SChannel TLS), with the destination's connection opened when the first packet
   is due (a stream that starts delayed starts that much later on the platform).

### Changing the delay live

The sender **splices**: it jumps to the newest video keyframe that is at least *delay* old and
rewrites timestamps so the next frame follows the last one sent by one frame interval. Adding delay
jumps back: viewers see the last seconds again, never anything newer. Removing it jumps forward.
The destination sees one continuous stream: no reconnect, no new header. The step is the keyframe
interval (2 s recommended); the dock warns above 4.5 s.

The history always reaches back `max(delay, kept)` seconds plus two keyframe intervals, so raising
the delay up to "Keep for instant raises" (default 120 s) needs no wait. Asking for more than the
history holds makes the sender hold until the oldest kept keyframe is old enough
(`SpliceInfo::shortHistory`).

### Network problems

- A destination that can't connect or send retries with back-off (2, 4, 8, 15 s) and a fresh
  timeline, at the delay. The others carry on.
- A destination that falls more than 4 s behind its delay (slow upload) jumps to the delay again at
  the next keyframe, instead of letting the delay grow (`resyncs` in the status).
- OBS's own congestion handling only sees the loopback, so OBS's dropped-frames counter stays at 0
  in stream mode; the dock shows each destination's state and rate instead.

### Stopping

When OBS stops, the relay keeps sending what viewers haven't seen yet (the last *delay* seconds),
unless "When I stop streaming, send the seconds viewers haven't seen" is off; the dock offers "Stop
sending now". A new stream, or closing OBS, cuts it.

### Dump

"Dump what viewers haven't seen" (dock, hotkey, obs-websocket `DumpDelay`) sets a splice floor at
now on the history (`PacketHistory::SetSpliceFloor`) and the delay to 0, under the lock the senders
take before `Due()`. Every sender re-splices to the first keyframe that arrives after the floor
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

## Controls

- Dock: on/off, back to live, seconds (1-600, presets, +/-5), destinations, kept seconds, drain.
- Hotkeys (OBS Settings > Hotkeys): Toggle, Enable, Disable (names unchanged), Back to live,
  Delay +5 s, Delay -5 s. They go through `DelayRouter`: stream mode when it handles the controls,
  scene mode otherwise.
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`; event
  `StatusChanged`. For the Trigglow Stream Deck plugin.

The free-account gate applies to Enable as in scene mode.

## Not covered yet

- macOS and Linux: the plugin's FFmpeg there has no avformat yet, so stream mode is unavailable
  and scene mode is used.
- Enhanced Broadcasting (multitrack video), SRT, RIST and WHIP services: scene mode.
- Overlay in stream mode: the delay badge is a scene mode feature for now.

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
  - Multistream: the service plus an extra destination get the same stream; a destination that
    can't connect retries without disturbing the others and doesn't hold up the end of a drain.
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
  the relay).
- Not run yet: a real platform (Twitch RTMP, Kick RTMPS, YouTube HEVC over enhanced RTMP) with
  its player watched, the dock UI by hand, and OBS reconnecting to the relay mid-stream (the relay
  listens once: if OBS's connection to it drops, the stream ends instead of resuming).
