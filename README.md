*[Versión en español](README-es.md)*

# Trigglow Dynamic Delay for OBS

Native OBS Studio plugin that delays what your viewers see by a configurable number of seconds,
**video and audio together**, from the dock, a native OBS hotkey, obs-websocket or a Stream Deck,
**without your stream ever reconnecting.** No external app, no web panel, no separate process:
everything lives inside the OBS process, and your stream goes straight from your computer to your
platform.

**Status: 1.0.0. Stable on Windows. The macOS and Linux builds are alpha: they build and pass CI
but haven't been run on a live stream yet, and they only have scene mode.**

## Two ways to delay

**Stream mode (new in 1.0.0, Windows, the default).** The plugin holds the stream OBS has already
encoded and sends it to your platform later. Nothing is decoded or encoded again, so it costs
almost no CPU and its RAM is your bitrate times the seconds kept: about 45 MB per minute at
6 Mbps, 90 MB for the default 120 s kept. With it:

- 1 to 600 s of delay, turned on, off, up or down while you're live, with no reconnect. Turning it
  on, viewers see the last seconds again, never anything newer.
- Every scene is delayed, so switching scenes while delayed is safe.
- Recording, the Replay Buffer and the virtual camera stay live.
- Multistream: extra rtmp:// or rtmps:// destinations get the same delayed stream from one encode.
- "Dump what viewers haven't seen": the seconds still waiting are never sent, the stream jumps to
  live and the delay comes back on its own.
- Delay by scene (on in your game scene, off in Just Chatting), three presets, the delay badge for
  viewers, and checks of the OBS settings that make it worse (keyframe interval, OBS's own Stream
  Delay).
- When you stop streaming, the seconds viewers haven't seen are still sent (optional).

It works with OBS's own RTMP or RTMPS output and any service on it (Twitch, YouTube, Kick, TikTok,
custom servers). Enhanced Broadcasting, SRT, RIST and WHIP fall back to scene mode. Details:
`docs/STREAM_MODE.md`.

**Scene mode (every platform; "Scene mode (compatibility)" in the dock).** The plugin keeps your
rendered live scene in RAM, compressed (your GPU's hardware HEVC on Windows, MJPEG otherwise), and
plays it back 1 to 60 s later in a helper scene; the streaming output is never touched. It costs
more RAM and CPU (table below) but works with any output, and it is what macOS, Linux and the
streams stream mode can't relay use. Details: `docs/SPEC.md`.

## How it works

```
Stream mode (Windows)

  OBS scenes -> OBS encoder -> OBS RTMP output --rtmp://127.0.0.1:<port>--> relay (in OBS)
                                    |                                         |
                 recording, Replay Buffer, virtual camera           packet history in RAM
                 (live, untouched)                                  (no decode, no encode)
                                                                              |
                                                     one sender per destination, each sending
                                                     packet p at arrival(p) + delay, over
                                                     RTMP/RTMPS -> Twitch / YouTube / Kick / ...

Scene mode (all platforms)

  live scene -> video/audio delay filters -> RAM ring (HEVC or MJPEG) -> helper scene
             -> OBS program -> OBS encoders -> your service (output never touched)
```

- **`StreamDelayController`** (`src/stream-delay-controller.*`) watches OBS's frontend events. When
  a stream is about to start and it's eligible (OBS's `rtmp_output`, RTMP or RTMPS, not Enhanced
  Broadcasting), it reads your service's server and key, starts the relay, and points **that
  output** at the relay with a private service. Your saved stream settings are never changed.
- **`StreamRelay`** (`src/stream-relay.*`, FFmpeg's avformat, Windows only) listens on
  `127.0.0.1` only, stores the FLV packets as they arrive without decoding them, and runs one
  sender per destination, each with its own connection, retries and state.
- **`PacketHistory` / `SinkScheduler`** (`src/packet-delay-core.*`, pure and unit-tested) decide
  what each sender sends: a delay change splices at the nearest suitable keyframe and rewrites
  timestamps, so the platform sees one continuous stream.
- **`BufferModeController`** (`src/buffer-mode-controller.*`) runs scene mode: the delay filters,
  the ring buffer, the helper scene, the fill countdown and the degradation ladder.
- **`DelayRouter`** (`src/delay-router.*`) is the one entry point for hotkeys and obs-websocket:
  each command goes to stream mode when it handles the controls, to scene mode otherwise.
- The **dock** (`src/settings-ui.*`, `src/stream-mode-panel.*`) shows both, with stream mode on
  top and scene mode folded under it.
- **Sign-in** (`src/auth-*`, `src/api-client.*`, `src/win-http.*`, `src/token-store.*`): the free
  account that turning the delay on needs, over WinHTTP on Windows and libcurl on macOS and Linux.

The full map of files is in [Project structure](#project-structure).

## Earlier versions

1.0.0 is the first stable release; everything before was Early Access. In short: 0.3 compressed the
RAM buffer (MJPEG), 0.3.3 added the free-account gate, 0.4 added "Report a problem", the update
check and translations, 0.5 the delay badge, 0.6 1440p60 support, the adaptive quality ladder, Trigi
and the new sign-in, 0.7 the GPU's hardware HEVC for the buffer on Windows (about half the RAM) and
OBS 33's plugin locations. Every change, with figures: `CHANGELOG.md`.

Before changing scene mode, read `docs/SPEC.md` (how buffer mode works, and why changing OBS's own
stream delay live was tried and rejected) and `docs/BUILD_VALIDATION.md` (what was verified, and
what wasn't). For stream mode, `docs/STREAM_MODE.md`.

## Usage requirements

**OBS Studio 31.1.0 or newer**, on every platform. The plugin is built against the OBS 31.1.1
sources (see `buildspec.json`; since OBS 32.0, OBS refuses a plugin built against a *newer* OBS
than the one running), and from v0.7.1 it refuses to load in an older OBS, with
`requires OBS Studio 31.1.0 or newer, running X` in the OBS log; the Windows installer checks it
too. Tested live up to OBS **32.2.2** on Windows. OBS 33 (in release candidate) is supported by the
v0.7.1 install locations but not tested live yet. The minimum is set in one place,
`buildspec.json`'s `obsCompat`.

| Plugin | Minimum OBS | Tested up to | Win | macOS | Linux |
|---|---|---|---|---|---|
| 1.0.0 | 31.1.0 | 32.2.2 | Installer, `.zip` (OBS 33 location) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.2 | 31.1.0 | 32.2.2 | Installer, `.zip` (OBS 33 location) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.1 | 31.1.0 | 32.2.2 | Installer, `.zip` (OBS 33 location) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.0 | 31.1.0 | 32.2.2 | Installer, `.zip` ("Legacy" in OBS 33) | `.pkg` | `.deb`, Ubuntu 24.04 with OBS 32.2 only |

Linux: one x86_64 build for Ubuntu 24.04 LTS (OBS from the OBS PPA), Ubuntu 26.04 LTS and 26.10,
Fedora, Arch and their derivatives (`.deb`, `.rpm`, `.pkg.tar.zst`), and a per-user `.tar.xz` for
the rest, with Qt 6.4 or newer. Debian 13 ships OBS 30.2, below the minimum: the `.deb` refuses to
install until OBS 31.1 or newer is there. The Flatpak OBS isn't supported. Details in
`docs/INSTALL_GUIDE.md`.

**Resolution and frame rate.** Any canvas: the delay buffer follows OBS's **output** resolution
(`Settings → Video → Output (Scaled) Resolution`) and never goes above it, so a canvas larger than
the output costs the plugin nothing extra. 1080p and **1440p outputs at up to 60 fps** are
supported since v0.6.0 (figures in `CHANGELOG.md`, measured offline on an 8-core/16-thread CPU). On
a machine that can't keep up, the buffer drops to a lower resolution (never below the minimum
quality you choose) and, last, to half the capture frame rate, instead of overloading OBS or cutting
the delay; the dock shows an amber notice while that happens.

**Memory in stream mode.** The bitrate of your stream times the seconds kept ("Keep for instant
raises", 120 s by default, or the delay if it's longer), plus two keyframe intervals. Nothing else
grows with the delay:

| Bitrate | 30 s | 60 s | 120 s (default kept) | 600 s |
|---|---|---|---|---|
| 3 Mbps | 11 MB | 22 MB | 45 MB | 225 MB |
| 6 Mbps | 22 MB | 45 MB | 90 MB | 450 MB |
| 8 Mbps | 30 MB | 60 MB | 120 MB | 600 MB |

The dock shows the RAM in use and the bitrate while you stream.

**Memory in scene mode.** The buffer's RAM budget is half of your system RAM, between 1 GB and 24 GB (on macOS and
Linux, where RAM isn't detected yet, a fixed 2 GB). What the buffer really takes depends on what's
on screen and on how it's stored. Measured outside OBS with gameplay-like content at 60 fps (the
buffer only; audio adds about 11 MB per source with sound for 30 s):

| Output | 30 s, hardware HEVC (Windows, v0.7.0) | 30 s, MJPEG | 60 s, hardware HEVC | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1,200 MB |
| 2160p60 | ~590 MB | ~1,250 MB | ~920 MB | ~2,300 MB |

The hardware figures are from an AMD GPU (AMF); NVIDIA's NVENC isn't measured yet. MJPEG is what
Linux, Windows without a hardware encoder, and the `buffer_codec` support setting use. If the
seconds you ask for don't fit at your minimum quality, fewer seconds are buffered (the dock tells
you before you enable it). Compression needs FFmpeg, bundled on Windows and Linux; on macOS the
buffer is stored uncompressed. Details in `docs/FAQ.md`.

**A free trigglow.com account.** Turning the delay on needs you to sign in once from the dock (in
your browser; the plugin stays 100% free), in both modes. Since 1.0.0 sign-in works on macOS and
Linux too (over the system's libcurl); before, it was Windows only. See `docs/ACCOUNT_GATE.md`.

**For stream mode:** Windows, OBS's own RTMP or RTMPS output (the usual "Stream" settings with any
RTMP service or a custom rtmp:// or rtmps:// server), Enhanced Broadcasting off. A keyframe
interval of 2 s is recommended (Settings > Output): the delay moves in steps of it.

## Network use and privacy

- **On every OBS start**: one check of your sign-in with `api.trigglow.com` (plus a renewal when
  it's due) and one check of this repository's latest release on `api.github.com`. Nothing else
  runs in the background.
- **Only when you use them**: signing in and out, asking Trigi and sending a report, all to
  `api.trigglow.com`. The plugin opens only its own fixed pages on www.trigglow.com and this
  repository on GitHub, in your browser.
- **How**: HTTPS only, redirects never followed, the sign-in sent only to `api.trigglow.com`,
  stored encrypted for your Windows user (on macOS and Linux, in a file only your user can read,
  mode 0600), and never written to the OBS log, like the sign-in codes, IDs and what you write to
  Trigi.
- **Stream mode** sends your stream from your computer straight to your platform and the extra
  destinations you add: the relay listens on `127.0.0.1` only, and no Trigglow server is in the
  path. The extra destinations' stream keys are saved on this computer in the plugin's
  `stream-mode-settings.json`, the same way OBS saves your own key in its `service.json`.
- **Trigi** receives what you type and, only if you tick it, a small set of OBS technical numbers
  and states (never names, paths, URLs or keys), shown before you send it. **Your OBS log is never
  sent to the AI**; it's attached to a ticket only if you tick its box, which you can do once
  you've seen it in **See what is sent**. Your Windows user name, URLs, keys, emails and IPs are
  hidden in it, but names of scenes, sources, audio devices and windows can still show up.
  Conversations are kept for up to 12 months and can be deleted earlier on trigglow.com. Details
  in `docs/FAQ.md` and the
  [Privacy Policy](https://www.trigglow.com/legal/privacy).

## Build requirements

Based on the official [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate)
template, without modifying its build system (`cmake/`, `CMakePresets.json`, `.github/`) except to
enable `ENABLE_FRONTEND_API` and `ENABLE_QT` and register the new source files.

| Platform | Tools |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Linux (Ubuntu 24.04) | CMake ≥ 3.28.3, Ninja, GCC 13, `nasm`, `jq`, Qt 6.4, libcurl (`libcurl4-openssl-dev`): `.github/scripts/linux/install-build-deps.sh` |

macOS links the system's libcurl (macOS 12's 7.79 is handled). **Windows is the priority platform
for 1.0.0**: macOS and Linux build green in CI on the template's own build system, but haven't been
run live in this release, and the sign-in over libcurl has been built by CI but not run on a real
Mac or Linux machine yet.

## Build (Windows)

```powershell
git clone <your-repo>/obs-trigglow-dynamic-delay.git
cd obs-trigglow-dynamic-delay
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The first configuration step will automatically download the OBS sources and the pre-built
dependencies (`obs-deps`, Qt6) declared in `buildspec.json` — you don't need to install OBS or Qt
yourself separately. Requires an internet connection the first time.

## Build (macOS / Linux)

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

On Linux the plugin links its own static FFmpeg and builds libobs from the pinned OBS sources on
the first configure, as CI does in an `ubuntu:24.04` container:

```bash
sudo .github/scripts/linux/install-build-deps.sh   # Ubuntu 24.04 packages
.github/scripts/linux/build-ffmpeg.sh              # FFmpeg from buildspec.json, LGPL, MJPEG only
.github/scripts/linux/build.sh RelWithDebInfo      # stages release/stage/usr
.github/scripts/linux/package.sh                   # .deb, .rpm, .pkg.tar.zst, .tar.xz
```

## Installing the plugin

**Windows: use the installer.** Download `obs-trigglow-dynamic-delay-1.0.0-windows-x64-setup.exe`
from the [latest release](https://github.com/TrigglowStudios/trigglow-dynamic-delay/releases/latest)
and run it — it installs into OBS's plugin folder for all users and moves an install of v0.7.0 or
earlier out of OBS's own folder. It isn't code-
signed yet, so Windows SmartScreen will warn you before letting it run. **Full guide
with screenshots for every step, including the SmartScreen warning: `docs/INSTALL_GUIDE.md`.**
Install it before you go live, not mid-stream — it needs OBS closed to overwrite its files.

**Linux: use your distro's package.** `obs-trigglow-dynamic-delay-1.0.0-x86_64-linux-gnu.deb`
(Debian, Ubuntu, Mint, Pop!_OS: `sudo apt install ./<file>.deb`), `.rpm` (Fedora, openSUSE:
`sudo dnf install ./<file>.rpm`) or `.pkg.tar.zst` (Arch, Manjaro, EndeavourOS:
`sudo pacman -U <file>.pkg.tar.zst`), which pull in what they need. Any other distro: the `.tar.xz`
and its `./install.sh`, for your user only.

**macOS:** the `.pkg` from the same release.

**Where the files go (v0.7.1):**

| Platform | OBS 33 and later | OBS 31 and 32 (legacy location) |
|---|---|---|
| Windows | `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\obs-trigglow-dynamic-delay.dll`, `data\` next to it | `...\obs-trigglow-dynamic-delay\bin\64bit\obs-trigglow-dynamic-delay.dll`, same `data\` |
| Linux, package | `<libdir>/obs-modules/plugins/obs-trigglow-dynamic-delay.so`, `/usr/share/obs/obs-modules/plugins/obs-trigglow-dynamic-delay/` | symlinks in `<libdir>/obs-plugins/` and `/usr/share/obs/obs-plugins/` |
| Linux, `.tar.xz` | `~/.local/share/obs-studio/plugins/obs-trigglow-dynamic-delay/` | symlinks in `~/.config/obs-studio/plugins/obs-trigglow-dynamic-delay/` |
| macOS | `~/Library/Application Support/obs-studio/plugins/obs-trigglow-dynamic-delay.plugin` | the same |

`<libdir>` is `/usr/lib/x86_64-linux-gnu` (`.deb`), `/usr/lib64` (`.rpm`) or `/usr/lib`
(`.pkg.tar.zst`). OBS 33 loads the first column and skips the legacy copy; OBS 31 and 32 only know
the second. On Windows each copy has its own FFmpeg DLLs next to it.

**Manual install on Windows:** extract the `.zip` into `C:\ProgramData\obs-studio\plugins\` (it
contains the `obs-trigglow-dynamic-delay` folder) and restart OBS. Don't copy anything into
`C:\Program Files\obs-studio`: OBS 33 marks plugins there as "Legacy" and OBS 34 won't load them.

## Using the plugin

Open OBS. If the **"Trigglow Dynamic Delay"** dock doesn't show up, go to `View → Docks` and enable
it. Sign in once with your free Trigglow account from the dock.

### Stream mode (Windows)

1. Leave **Use stream mode** ticked (the default). Set the **Delay** in seconds (1-600) and, if you
   want, the three **presets** (10, 30 and 60 s by default).
2. Start streaming as usual. With the delay off you're live; press **Turn delay on** at any moment
   and viewers see the stream that many seconds late. If you turn it on before you start
   streaming, the stream starts that many seconds late on the platform ("Delay armed").
3. Change the seconds while live: raising them rewinds (viewers see the last seconds again),
   lowering them skips forward. **Back to live** turns the delay off. Up to **Keep for instant
   raises** (120 s by default) a raise needs no wait.
4. **Dump what viewers haven't seen** when something slipped on screen: the seconds still waiting
   in the delay are never sent, the stream jumps to live, and the delay comes back on its own
   after its seconds.
5. Optional: **Destinations** for multistream (name, rtmp:// or rtmps:// server, key), **Delay by
   scene** (turn the delay on with its own seconds, or off, when you switch to a scene), **Show the
   delay badge to viewers** and its corner, and **When I stop streaming, send the seconds viewers
   haven't seen** (on by default; **Stop sending now** cuts it).

The dock shows the state ("Live, no delay", "Delay on: viewers see the stream 30 s late"...), each
destination's state and data sent, the RAM kept and the bitrate, and the OBS settings worth changing.
The delay moves in steps of your keyframe interval: set it to 2 s in `Settings → Output`. If a
stream can't use stream mode (Enhanced Broadcasting, SRT, WHIP...), the dock says why and scene mode
works as before.

### Scene mode (every platform)

Under **Scene mode (compatibility)** in the dock (it is the only mode on macOS and Linux):

1. Pick your **live scene** (required — the scene with your real content) and, optionally, a
   **loading scene** to show while the buffer fills instead of staying on the undelayed live scene
   (if you choose none, a Trigglow "Adding delay" video shows: "Trigglow (default)" in the picker).
   Set **Delay (seconds)** (1–60) and the **minimum quality** floor (480p, 720p, 1080p, 1440p (2K)
   or 2160p (4K)) the delayed video won't drop below to save RAM (it's never stored above OBS's
   output resolution either: with a 720p output the buffer is 720p even with a 1080p minimum). The
   dock shows a live estimate of whether your chosen seconds actually fit in RAM at that quality
   *before* you press Enable — if they don't, it warns that the buffered duration will be
   shortened instead of lowering the quality further, but never blocks your choice. These fields
   are locked while the delay is preparing or on ("Turn the delay off to change it.").
2. Press Enable (from the dock button, a hotkey, or Stream Deck). The dock shows **Preparing
   delay** with a progress bar and a live countdown while the buffer fills up to the requested
   delay; once full, it switches automatically to **Delay on** and Program starts showing the
   delayed video and audio, in sync. **The streaming output itself is never touched at any point**
   — viewers never see a cut or a reconnect, whether you press the button before or during a live
   stream.

Pressing Disable frees the RAM buffer immediately and switches back to whatever scene was showing
before you enabled it.

Scene mode manages a few objects in your scene collection by itself: the scene **Trigglow Delay
Buffer (auto)**, the filters **Trigglow Buffer Mode Delay (auto)** and `Trigglow Audio Delay (auto) -
<source>`, the overlay image **Trigglow Delay Badge (auto)** and, when you choose no loading scene,
the scene **Trigglow Delay Loading (auto)**. You can move or resize the overlay in OBS and it stays
where you put it; don't rename or delete the others. Stream mode adds nothing to your scene
collection (its badge is a private source on an OBS output channel).

### Hotkeys

In `Settings → Hotkeys`, search for "Trigglow". They work in both modes (stream mode when it
handles the current or next stream, scene mode otherwise):

| Hotkey | Stream mode | Scene mode |
|---|---|---|
| Trigglow: Toggle Dynamic Delay | delay on/off | Enable/Disable |
| Trigglow: Enable Dynamic Delay | delay on | Enable |
| Trigglow: Disable Dynamic Delay | delay off | Disable |
| Trigglow: Back to live (delay off) *(new)* | delay off | Disable |
| Trigglow: Delay +5 s / Delay -5 s *(new)* | changes the delay live | changes the seconds for the next Enable (1-60) |
| Trigglow: Dump what viewers haven't seen *(new)* | dump | nothing |
| Trigglow: Delay preset 1 / 2 / 3 *(new)* | the preset's seconds, delay on | the preset's seconds (up to 60), Enable |

The three original hotkeys keep their internal names, so your bindings carry over. In Spanish they
read "Trigglow: Activar/desactivar Dynamic Delay", "Activar" and "Desactivar".

### obs-websocket

The plugin registers an obs-websocket 5 vendor, **`trigglow-dynamic-delay`**, when obs-websocket
is loaded (it ships with OBS). Send `CallVendorRequest` with `vendorName: "trigglow-dynamic-delay"`
and one of these `requestType`s:

| Request | Data | Does |
|---|---|---|
| `GetStatus` | — | only returns the status |
| `EnableDelay` / `DisableDelay` / `ToggleDelay` | — | as the hotkeys |
| `SkipToLive` | — | back to live (delay off) |
| `SetDelay` | `{"seconds": N}` | sets the seconds (stream mode 1-600, scene mode 1-60); 0 or less is ignored |
| `AdjustDelay` | `{"delta": N}` | adds N seconds (negative to lower) |
| `DumpDelay` | — | dump (stream mode); `dumped` says whether there was anything to drop |
| `ApplyPreset` | `{"index": 0-2}` | preset 1-3 |

Every answer carries `ok` and the status: `mode` (`stream` or `scene`), `state` (stream:
`idle`, `live`, `delayed`, `draining`; scene: `inactive`, `filling`, `active`, `error`), `delayOn`,
`delaySeconds`, `appliedSeconds` (what viewers get now, 0 when live), `behindSeconds` and
`ramMegabytes` (stream mode). The vendor event **`StatusChanged`** carries the same fields without
`behindSeconds` and `ramMegabytes`, sent when one of them changes.

## Mapping a Stream Deck button

Two ways:

- **Trigglow's Stream Deck plugin** talks to OBS over obs-websocket (the vendor above) and shows the
  delay's state on its keys. Its Dynamic Delay keys (toggle, back to live, presets, +/-) are being
  added to that plugin; see `docs/STREAM_DECK.md`.
- **Any Stream Deck, no plugin:** add the **"System" → "Hotkey"** action and send the key
  combination you bound to one of the hotkeys above. OBS needs to be running and listening for the
  global shortcut (with some setups, focused).

Pressing it while live is safe in both modes: no reconnection and no visible cut. More detail and
troubleshooting: `docs/STREAM_DECK.md`.

## Project structure

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring for all components, the
                                     settings files (buffer-mode-settings.json,
                                     stream-mode-settings.json)
  stream-delay-controller.{hpp,cpp} → v1.0.0 stream mode, OBS side: eligibility, pointing the
                                     stream output at the relay, delay on/off/seconds, dump,
                                     presets, scene rules, the badge, the settings checks
  stream-relay.{hpp,cpp}           → the relay (FFmpeg avformat, Windows): RTMP listener on
                                     127.0.0.1, one sender per destination, retries, drain
  packet-delay-core.{hpp,cpp}      → pure: PacketHistory and SinkScheduler (splices, timestamp
                                     rewrite, trimming, dump floor), unit-tested
  stream-mode-panel.{hpp,cpp}      → the dock's stream mode section
  delay-router.{hpp,cpp}           → hotkeys and obs-websocket to stream mode or scene mode
  websocket-vendor.{hpp,cpp}       → the obs-websocket vendor trigglow-dynamic-delay (requests,
                                     StatusChanged event)
  core-host.{hpp,cpp}, core-main.cpp → the core-only build (TRIGGLOW_CORE_ONLY): scene mode driven
                                     through a source's settings, for hosts without OBS's frontend
  buffer-mode-controller.{hpp,cpp} → the state machine (Inactive/Filling/Active/Error) that
                                     orchestrates buffer mode — the only owner of state
  obs-frontend-bridge.{hpp,cpp}    → the only layer that touches obs-frontend-api.h + filter/
                                     scene wiring
  video-delay-filter.{hpp,cpp}     → the video delay filter: capture, I420 conversion on the GPU,
                                     readback, playback, its small fixed GPU object pool, and the
                                     perf check / pipeline check telemetry
  codec-pipeline.{hpp,cpp}         → encode/decode worker threads, the storage codec's setup thread
                                     and the reaper thread, off OBS's graphics thread,
                                     stress-tested in tests/
  storage-codec.hpp                → the storage codec interface (asynchronous encoder, decoder),
                                     the raw codec, NV12 helpers and the chain bookkeeping of
                                     inter-frame codecs
  hw-storage-codec.{hpp,cpp}       → v0.7.0, Windows only: hardware HEVC/H.264 storage codec
                                     (NVENC, AMF, Quick Sync) with D3D11VA decode, and its probe
  storage-codec-select.{hpp,cpp}   → pure: the hidden buffer_codec setting, OBS encoder families,
                                     the ranking of hardware encoders, unit-tested
  storage-codec-policy.{hpp,cpp}   → pure: when the hardware codec runs and when the buffer goes
                                     back to MJPEG ("Encoding overloaded" guard), unit-tested
  annexb.{hpp,cpp}                 → pure Annex B walk: parameter sets in band, unit-tested
  mjpeg-codec.{hpp,cpp}            → MJPEG over the vendored FFmpeg, one instance per thread
  frame-ring.{hpp,cpp}             → ring of slot headers with a state machine and per-packet
                                     codec, key, chain and seq, unit-tested
  frame-arena.{hpp,cpp}            → block arena for the compressed frames, unit-tested
  degradation-policy.{hpp,cpp}     → pure adaptive degradation (threads, resolution rungs, half
                                     rate), unit-tested
  pipeline-health.hpp              → what the dock reads about the buffer's degradation
  perf-stats.{hpp,cpp}             → fixed-size percentile series for the telemetry
  buffer-fit.{hpp,cpp}             → pure buffer sizing math (output-resolution cap, quality
                                     floor, RAM budget), unit-tested in tests/
  encoder-check.{hpp,cpp}          → pure decisions about OBS's encoders: which see the main
                                     canvas, the "encoder above the buffer" log warning and the
                                     encoders' ceiling on the buffer (v0.7.0), unit-tested
  audio-delay-filter.{hpp,cpp}     → per-leaf-source audio delay ring, kept in sync with video
  hardware-info.{hpp,cpp}          → total system RAM query, used to size the buffer budget, and
                                     the GPUs present (vendor, for the hardware storage codec)
  scene-combo-box.{hpp,cpp}        → dock combo box populated from OBS's own scene list
  settings-ui.{hpp,cpp}            → the Qt dock (live/loading scene, seconds, quality, fit
                                     estimate, Enable/Disable, live fill countdown, degradation
                                     notice, update notice)
  ui-tokens.{hpp,cpp}              → pure color tokens derived from OBS's palette, with the WCAG
                                     AA contract unit-tested in tests/
  ui-theme.{hpp,cpp}               → the UI's single style sheet, bundled brand fonts and
                                     recolored SVG icons (src/resources/), shared by dock and dialog
  overlay-badge.{hpp,cpp}          → paints the live "DELAY 30 s" overlay as one PNG (QPainter)
  overlay-layout.{hpp,cpp}         → pure placement of that overlay on the canvas, unit-tested
  auto-names.{hpp,cpp}             → names of the scene/filters/overlay the plugin manages, and
                                     their migration from the v0.5.x "(no tocar)" names, unit-tested
  report-bug-dialog.{hpp,cpp}      → "Report a problem" dialog: Trigi first when available, then
                                     the ticket form (signed-in or guest), with the redacted OBS
                                     log only if the user ticks it after seeing it
  trigi-page.{hpp,cpp}             → Trigi's page in that dialog: notice, technical data,
                                     conversation, states and the hand-off to a ticket
  trigi-widgets.{hpp,cpp}          → the conversation view (never loads anything) and its bubbles
  trigi-states.{hpp,cpp}           → pure state, button and countdown decisions, unit-tested
  chat-text.{hpp,cpp}              → pure sanitizer and tiny markup parser for Trigi's answers
                                     (escaped text, bold, code, lists only), unit-tested
  support-client.{hpp,cpp}         → Trigi and ticket calls, answers and errors, unit-tested
  support-service.{hpp,cpp}        → the Qt side of support: background threads, one Trigi
                                     request at a time, local choices, log lines
  support-diagnostics.{hpp,cpp}    → the optional OBS technical data (numbers, booleans and
                                     fixed values only, within 1 KB), unit-tested
  diagnostics-collector.{hpp,cpp}  → fills that snapshot from OBS and the plugin
  bug-report.{hpp,cpp}             → finds this OBS's current log (Windows only for now: the one
                                     holding a marker the plugin writes at load), reads and
                                     redacts it in memory, and deletes old copies in %TEMP%
  log-redact.{hpp,cpp}             → pure, line-by-line redaction of the OBS log, unit-tested
  update-checker.{hpp,cpp}         → checks this repo's latest GitHub release against the running
                                     version
  background-tasks.{hpp,cpp}       → cancellable background jobs (sign-in, Trigi, tickets, update
  cancel-token.{hpp,cpp}             check) with a bounded shutdown when OBS unloads the plugin,
  ui-tasks.{hpp,cpp}                 unit-tested; ui-tasks ties them to Qt's event loop
  auth-manager.{hpp,cpp}           → the free-account login gate (see `docs/ACCOUNT_GATE.md`),
                                     on background threads
  auth-client.{hpp,cpp}            → sign-in protocol: device link v2, renewal, sign-out, the
                                     one-time v0.5.x exchange, 401/403 rules, unit-tested
  token-store.{hpp,cpp}            → the plugin's sign-in, encrypted with the Windows data
                                     protection API; on macOS and Linux a user-only (0600)
                                     file, unit-tested
  api-client.{hpp,cpp}             → requests to api.trigglow.com only, errors, Retry-After and
                                     the fixed pages the plugin may open, unit-tested
  json.{hpp,cpp}                   → strict, bounded JSON reader/writer, unit-tested
  win-http.{hpp,cpp}               → the one hardened HTTPS path (no redirects, TLS 1.2+, port
                                     443 only, timeouts, size cap) for every network call:
                                     WinHTTP on Windows, libcurl on macOS and Linux
  i18n.hpp                         → Str() helper (obs_module_text()) — every user-facing string
                                     in this plugin goes through it; see data/locale/*.ini
  hotkeys.{hpp,cpp}                → registration of the 10 native OBS hotkeys, through
                                     DelayRouter
  logging.{hpp,cpp}                → logging wrapper with a component prefix
  delay-controller.{hpp,cpp}       → legacy reconnect-mode logic from the abandoned first design
                                     (see `docs/SPEC.md` §6) — present in the repo but never
                                     instantiated by plugin-main.cpp; ships in no current build
data/
  locale/en-US.ini, es-ES.ini,     → the plugin's translated strings (see src/i18n.hpp); OBS
  pt-BR.ini, zh-CN.ini               picks the one for its language, English for missing keys
  images/, videos/                 → the fallback badge frame and the default loading videos
  fonts/                           → bundled Figtree and Geist Mono + their OFL licenses
  licenses/                        → notices of third-party components (Lucide icons, FFmpeg DLLs)
tests/
  buffer_fit_tests.cpp             → unit tests for buffer-fit (off by default:
                                     -DTRIGGLOW_BUILD_TESTS=ON, then ctest)
  encoder_check_tests.cpp          → unit tests for encoder-check (same)
  ui_tokens_tests.cpp · auto_names_tests.cpp · overlay_layout_tests.cpp ·
  frame_arena_tests.cpp · frame_ring_tests.cpp · degradation_policy_tests.cpp ·
  storage_codec_tests.cpp · storage_policy_tests.cpp → same, for those modules
  pipeline_stress_tests.cpp        → concurrency stress test of ring + arena + codec pipeline
  pipeline_gop_stress_tests.cpp · adversarial_gop_tests.cpp → the same with a fake inter-frame
                                     codec (fake-gop-codec.hpp): chains, losses, evacuations,
                                     yields, render pauses
  hw_storage_codec_tests.cpp       → the hardware storage codec over the real FFmpeg, failures
                                     injected (Windows, local builds with the plugin's FFmpeg)
  json_tests.cpp · log_redact_tests.cpp · chat_text_tests.cpp · support_diagnostics_tests.cpp ·
  api_client_tests.cpp · token_store_tests.cpp · auth_client_tests.cpp ·
  support_client_tests.cpp · trigi_states_tests.cpp → account, Trigi and ticket client, with
                                     a scripted network stand-in (fake-transport.hpp)
  background_tasks_tests.cpp       → cancellation and bounded shutdown of the background jobs
  packet_delay_core_tests.cpp      → stream mode's splicing, timestamps, trimming and dump
  relay_harness.cpp                → end-to-end harness for the relay: a local RTMP stream in,
                                     delay changes, a dump, the output checked (manual, Windows)
tools/fonts/                       → how the bundled font subsets are generated
docs/
  SPEC.md                  → full technical specification (start here)
  INSTALL_GUIDE.md          → step-by-step install walkthrough with screenshots (SmartScreen,
                             installer wizard, enabling the OBS dock)
  BUILD_VALIDATION.md       → what was actually verified in this release, and what wasn't
  ROADMAP.md                → what each version shipped, what's next, and what's out of scope
  STREAM_MODE.md            → stream mode: how it works, controls, limits, what's verified
  STREAM_DECK.md             → detailed Stream Deck guide, hotkeys and obs-websocket
  FAQ.md · TROUBLESHOOTING.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → the official template's build system
```

## License and repository

GPL-2.0-or-later, inherited from `obsproject/obs-plugintemplate` (see `LICENSE`) — this isn't just
a formality: `libobs` is GPL-2.0, so linking against it carries the same license over to this
plugin, which requires publishing the source code of any distributed binary.

That's why this project lives in **its own repository, separate from Trigglow's main repository**
(which stays private) — publishing only the plugin here satisfies the GPL without exposing the
rest of the product. Full detail on this decision in `docs/SPEC.md` §9.

`main` is a protected branch on GitHub: merges require a pull request, even for the repo owner (no
direct pushes, no force-push, no branch deletion). Day-to-day work happens on `develop`; PRs go
`develop` → `main`.

### Third-party assets

The plugin ships a few third-party components, each under its own license (all compatible with
redistribution next to a GPL-2.0 binary):

| Asset | Where | License |
|---|---|---|
| **Figtree** (SemiBold, Bold), subset | `data/fonts/Figtree-*.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-Figtree.txt` |
| **Geist Mono** (Medium), subset | `data/fonts/GeistMono-Medium.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-GeistMono.txt` |
| Interface icons (Lucide paths, as in the Trigglow web app) | compiled in from `src/resources/icons/` | ISC (portions MIT, Feather), `data/licenses/LICENSE-lucide.txt` |
| **FFmpeg** libraries (avcodec, avutil, swresample, and avformat for stream mode), BtbN LGPL build pinned in `buildspec.json`, Windows only | DLLs next to the plugin's own (`bin/64bit` in the package), unmodified | LGPL 3.0 or later, with MIT/BSD/Apache components (libvpl, AMF, ffnvcodec); `data/licenses/LICENSE-ffmpeg.txt`, with the build's tag and its source |

The fonts are static instances subset from the upstream variable fonts in
[google/fonts](https://github.com/google/fonts) (`ofl/archivo`, `ofl/geistmono`); `tools/fonts/`
documents how. The OFL requires its text to travel with the fonts, so each `OFL-*.txt` sits next to
its font in the package and in the installer. The fonts are only used by the plugin's own dock,
dialog and overlay (OBS's UI font is never changed); without them every text falls back to OBS's
font.
