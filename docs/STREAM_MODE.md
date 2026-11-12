*[Versión en español](STREAM_MODE-es.md)*

# Stream mode (v1.0.0)

Stream mode delays the stream OBS has **already encoded**, instead of the rendered frames. In the
dock it is the advanced option **"Apply the delay only to the stream (recording and preview stay
live)"** (Options > Advanced), off by default: by default the delay applies to all of OBS, with
scene mode (buffer mode, `docs/SPEC.md`), as in 0.7. Stream mode runs wherever its relay exists
(Windows, and macOS and Linux as alpha).

## One panel, two engines

The dock is one panel with one "Turn delay on" button. The check box picks the engine, saved as
`apply_to` (`"all"` or `"stream"`) in `stream-mode-settings.json` (`src/apply-to.*`):

- **All of OBS** (`"all"`, the default, also when the key is missing or not one of the two values):
  scene mode. The loading scene shows while the buffer fills, then Program, so the stream and the
  recording, are delayed. The dock shows the live and loading scene pickers, the minimum quality
  and the RAM estimate.
- **Only the stream** (`"stream"`): stream mode. The dock hides those and shows where the stream
  goes, Dump, the settings checks, Delay by scene, the delay buffer and sending the rest on stop.

The dock's button, the hotkeys, obs-websocket and the Stream Deck all go through `DelayRouter`
(`src/delay-router.hpp`), which sends each control to the selected engine only:
`StreamDelayController::HandlesControls()` is true when "only the stream" is chosen and the relay
exists here (`ControlsGoToStream()`); a stream that can't use the relay doesn't hand the controls
to scene mode, the dock says why instead. The check box is locked while either engine's delay is on
or draining ("Turn the delay off to change it."); ticked while OBS is already streaming, it applies
from the next stream. When it changes, the new engine starts from the seconds the dock shows; the
badge's on/off and corner are written to both. At load the engine that isn't selected takes the
selected one's seconds and badge.

1.0 test builds saved `mode_enabled` (true by default, "Use stream mode") instead. 1.0 was never
released, so that key is ignored and dropped on the next save, and those installs start on all of
OBS (`apply_to_tests` covers the migration and the router).

## Why it costs almost nothing

Scene mode, per frame: render, read back from the GPU, compress into RAM (MJPEG or the GPU's
HEVC), decompress later, render again, and then OBS encodes it for the stream. Its RAM is the
buffer at a quality close to the source: ~190 MB to ~2.3 GB for 30-60 s (README table).

Stream mode keeps the packets OBS's own encoder produced for the stream and sends them later.
Nothing is decoded or encoded again; the CPU copies bytes. The history is the stream's bitrate times
the seconds kept (45 MB per minute at 6 Mbps). The streamer chooses where it waits: in RAM, or
mostly in an encrypted temp file on disk, where the RAM it takes stays about the same at any delay
(see Memory and the disk).

## Memory and the disk

**Where the delay buffer lives** is the streamer's choice (dock: Options > Delay buffer, saved as
`buffer_storage` in `stream-mode-settings.json`), like a recorder's "memory / disk" buffer setting:

- **Memory:** the whole history stays in RAM and nothing is written to disk: the bitrate times the
  seconds kept (45 MB per minute at 6 Mbps). It reaches back the delay or the highest preset or
  delay-on scene rule (see Changing the delay live), so raises past that wait for the missing seconds.
- **Disk:** the spill below. RAM stays at 13-15 MB at any delay and every raise up to 600 s is
  instant, but the whole stream is written to the disk while streaming (about 2.7 GB per hour at
  6 Mbps), and read back for long delays.
- **Automatic** (the default, also for settings saved before the choice existed): Memory when the
  temp folder is on an SSD, Disk when it is on a spinning hard drive. An SSD gains little from the
  spill at the usual delays (a 60 s history is 45 MB of RAM) and would take hours of continuous
  writes for it; a hard drive doesn't wear from writes, and a machine with one is the likelier to
  be short of RAM. A drive whose type can't be told (a network drive, a USB bridge that doesn't
  pass it on, an error) counts as an SSD. The dock names what was detected: "Automatic (Memory, SSD
  detected)".
- The drive is checked when OBS loads (for the dock) and again at each stream start, and logged once
  per stream (`delay buffer: memory (choice auto, temp folder drive ssd)`), never with the path. A
  change while live applies to the next stream; the dock says so.
- **Detection** (`drive-kind.*`, the drive that holds the spill folder): on Windows the volume's
  disk(s) answer `IOCTL_STORAGE_QUERY_PROPERTY` for `StorageDeviceSeekPenaltyProperty` (a seek
  penalty is a hard drive), with `StorageDeviceTrimProperty` as the second signal when that can't
  be read (TRIM means SSD; no TRIM alone proves nothing). A volume over several disks is an HDD if
  any is. Network shares and RAM disks aren't asked. No administrator rights are needed. On Linux,
  `/sys/dev/block/<major:minor>/queue/rotational` of the folder's device (its disk for a
  partition; for a file system with an anonymous device number, like btrfs, the mount's source
  from `/proc/self/mountinfo`). On macOS, IOKit's "Medium Type" ("Solid State" / "Rotational") in
  the device characteristics, searched up the registry from the folder's BSD disk (through APFS
  containers to the physical drive).

The rest of this section is how the history is stored, and how Disk works:

- **Contiguous storage.** The packets' bytes go back to back into 1 MiB blocks
  (`PayloadStore`, `payload-store.*`), one record per packet: its flags, side data and payload,
  exactly what FFmpeg's demuxer returned, so the platform gets the same bytes. The scheduler's
  metadata is 48 bytes per packet. Before, every packet was its own FFmpeg allocation, which cost
  10-13% on top of the history (below: +490 MB of RAM for 446 MB of history, +106 MB for 93 MB).
- **The spill** (Disk). Only the newest seconds (4 s plus two keyframe intervals: live and short delays
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
- **Disk use.** The bitrate times 600 s while streaming with Disk (about 450 MB at 6 Mbps, 600
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
| 600 s, RAM only (Memory) | 607 s, 446 MB | | | +454 MB | 449 MB | 0 |

"+N MB" is how much the process grew (private bytes) over itself before the stream. "Before" is the
relay without these changes, with "Keep for instant raises" at its default (120 s); "Contiguous
blocks only" is the first change without the spill. Within OBS, the history is the only part of
stream mode that grows with the delay.

Tools that relay the stream outside OBS hold the same bytes: a 6 Mbps stream held for 60 s is 45 MB
wherever it is. Stream mode keeps it on disk, encrypted, and in RAM only what is about to be sent.

## How it works

1. On `OBS_FRONTEND_EVENT_STREAMING_STARTING` (after OBS created the stream output, before it
   starts), with "only the stream" chosen, `StreamDelayController` checks the stream is eligible: OBS's `rtmp_output`, protocol
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

With Disk (the spill) the history always reaches back 600 s (the most a delay can be) plus two
keyframe intervals plus 1 s: it costs disk, not RAM, and every live raise is instant once the stream
has run that long. With Memory, or when the disk couldn't be used, it reaches back the delay or the
highest preset or delay-on scene rule, whichever is higher, plus the same slack (`KeepSeconds()` and
`RetentionNs()` in `packet-delay-core.hpp`). The dock's "Instant raises: N s", next to "Delay
buffer", says which applies.
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
safe (all of OBS only delays the live scene picked in the dock).

## The delay badge

While the delay is on and the stream goes through the relay, the "DELAY N s" badge (the same PNGs
as scene mode, up to 600 s) is drawn on libobs output channel 61, over OBS's program after the
scene. Viewers get it with the delayed picture. Because it is on the program, the recording (and
anything else that takes the program) shows it too while the delay is on. On/off ("Show the delay
badge to viewers") and the corner are in the dock's Options, one control for both engines, and
saved.

## Settings checks

When a stream starts, the controller looks at OBS's settings and the dock lists what makes stream
mode worse (`StreamDelayController::CheckObsSettings`):

- the stream encoder's keyframe interval on auto (0, often 4-10 s) or above 4 s: the delay moves
  in steps of it;
- OBS's own Stream Delay (Settings > Advanced) on: its fixed delay adds up with this one.

## Controls

- Dock (with "only the stream" ticked): the one on/off button, seconds (1-600, +/-5, the presets
  as quick picks), dump, the three presets (Hotkey presets), delay by scene,
  the delay buffer (Automatic / Memory / Disk) with how far a raise is instant (read only; it
  replaced the "Keep for instant raises" setting, whose saved `keep_seconds` is still read and
  written back unchanged), drain, the badge, where the
  stream goes (read only: OBS's Settings > Stream). The stats show the RAM and the disk the history
  takes.
- Hotkeys (OBS Settings > Hotkeys): Toggle, Enable, Disable (names unchanged), Back to live,
  Delay +5 s, Delay -5 s, Dump, Preset 1-3. They go through `DelayRouter`, to the engine selected
  in the dock only (with all of OBS the seconds change live too, 1-600, a preset also turns the
  delay on, and Dump does nothing).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); event `StatusChanged`. For the Trigglow Stream Deck plugin; details
  in `docs/STREAM_DECK.md`.

The free-account gate applies to Enable as in scene mode.

## Not covered yet

- Enhanced Broadcasting (multitrack video), SRT, RIST and WHIP services: the dock says the stream
  can't use it; all of OBS (the default) works with them.
- Delay by scene only with "only the stream": all of OBS keeps delaying the one live scene chosen
  in the dock.

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
- The delay buffer choice (2026-11-11), Windows 11:
  - Unit tests: `drive_kind_tests` (Automatic with SSD, HDD and unknown drives, the explicit
    choices, saved values and settings from before the choice, the Windows signals and several
    disks, Linux's `rotational` and `mountinfo` parsing, macOS's medium type).
  - Detection on this machine (two NVMe SSDs, no hard drive): C: and E: SSD, the temp folder
    (on C:) SSD; drive letters that don't exist Unknown.
  - In a portable OBS 33.0.1: a settings file without `buffer_storage` loaded as Automatic and saved
    it back as `auto`; a stream to a local RTMP listener with Automatic logged
    `delay buffer: memory (choice auto, temp folder drive ssd)` and made no spill file, one with Disk
    logged `disk (choice disk, ...)` and the spill file was there while streaming. The dock at
    260-380 px in English, Spanish, Portuguese and Chinese: no clipping; "Instant raises" sits
    next to "Delay buffer" or under it when there's no room, and Automatic's long label ends in
    "..." when the combo is too narrow for it.
- Not run yet: a real platform (Twitch RTMP, Kick RTMPS, YouTube HEVC over enhanced RTMP) with
  its player watched, the spill at runtime on macOS (not built here) and Linux (unit tests only),
  drive detection at runtime on macOS and Linux and on a hard drive, a USB drive or a network temp
  folder, a slow (USB, HDD) or nearly full disk in practice, and OBS reconnecting to the relay mid-stream (the
  relay listens once: if OBS's connection to it drops, the stream ends instead of resuming).
- The relay and the OBS integration build on Windows, Linux and macOS. On Linux the relay itself ran
  end to end with `tests/relay_harness.cpp` (ffmpeg standing in for OBS and the platform) against
  the plugin's static FFmpeg: RTMP and RTMPS (certificate and host name checked by OpenSSL), the
  delay 0 -> 6 -> 0 s live, and the drain after the stop. macOS and Linux haven't run in a real OBS
  yet.
