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
2. It starts `StreamRelay` (FFmpeg avformat, Windows): a listener on `127.0.0.1:<free port>` and a
   sender to the streamer's service.
3. It points **that output** at the relay with a private `rtmp_custom` service. The streamer's saved
   service is untouched; OBS sets it on the output again at every start.
4. OBS publishes to the relay; the relay demuxes the FLV (no decoding) into `PacketHistory`
   (`packet-delay-core.*`), stamping each packet with its arrival time.
5. Each sender's `SinkScheduler` sends packet *p* once `now >= arrival(p) + delay`, remuxed to FLV
   over RTMP/RTMPS (SChannel TLS), with the platform connection opened when the first packet
   is due (a stream that starts delayed starts that much later on the platform).

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
  kept seconds (10-600), drain, the badge.
- Hotkeys (OBS Settings > Hotkeys): Toggle, Enable, Disable (names unchanged), Back to live,
  Delay +5 s, Delay -5 s, Dump, Preset 1-3. They go through `DelayRouter`: stream mode when it
  handles the controls, scene mode otherwise (there, +/-5 and presets change the seconds for the
  next Enable, clamped to 1-60, and a preset also turns the delay on; Dump does nothing).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); event `StatusChanged`. For the Trigglow Stream Deck plugin; details
  in `docs/STREAM_DECK.md`.

The free-account gate applies to Enable as in scene mode.

## Not covered yet

- macOS and Linux: the plugin's FFmpeg there has no avformat yet, so stream mode is unavailable
  and scene mode is used.
- Enhanced Broadcasting (multitrack video), SRT, RIST and WHIP services: scene mode.
- Delay by scene only in stream mode: scene mode keeps delaying the one live scene chosen in the
  dock.

## Verification status

- `packet-delay-core` is unit-tested (`tests/packet_delay_core_tests.cpp`): live passthrough,
  rewinding on enable with the delay held and monotonic timestamps, skipping forward on disable,
  short history, splicing before the first keyframe, trimming, audio only.
- The relay and the OBS integration build on Windows; a live stream to a real platform has **not**
  been run yet. To do before release: Twitch (RTMP), Kick (RTMPS), YouTube (HEVC over enhanced
  RTMP), a delay change and an OBS stop mid-delay, with the platform's player watched.
