*[Versión en español](ROADMAP-es.md)*

# Roadmap — Trigglow Dynamic Delay

## v0.2.0 (MVP — shipped)
- No-reconnect **buffer mode**: the streaming output is never touched, at any point, for any
  reason — the plugin delays video and audio by buffering them in system RAM and switching OBS's
  Program internally between the live scene, an optional loading scene, and a delayed wrapper
  scene.
- Enable / Disable / Toggle Delay, with the live scene required and the loading scene optional.
- Selectable minimum quality (480p/720p/1080p); requested duration shortens instead of ever
  lowering it, if the RAM budget doesn't fit both.
- Live buffer-fit estimate and detected RAM budget shown in the dock, updated as the user adjusts
  seconds or quality, with a non-blocking warning before Enable if the full request won't fit.
- Video and audio delayed together, in sync.
- RAM buffer freed automatically on Disable.
- `Inactive` / `Filling` (with a live countdown) / `Active` / `Error` status visible in a native
  OBS dock.
- 3 native OBS hotkeys (Toggle mandatory, Enable/Disable optional), directly usable from Stream
  Deck via its own "System: Hotkey" action.
- Settings persistence per OBS profile.
- Crash-free error handling (no live scene chosen, live scene resolves to nothing, etc.).
- Windows as the priority platform; macOS/Linux build green in CI but haven't been run live yet.

## v0.3.0 (shipped — 2026-08-27)
- Real buffer **compression**: each ring slot is now MJPEG-encoded (all-intra) on capture and
  decoded on playback, vendoring FFmpeg for both directions (`obs_video_encoder_create` only feeds
  OBS's own output pipeline, and libobs exposes no public decoder API — a GPU-compute-shader path
  was also investigated and ruled out for v0.2.0, since libobs has no compute/dispatch API in its
  public graphics interface). Automatic fallback to uncompressed NV12 whenever the codec isn't
  available or a frame fails.
- Ring slots reserve RAM based on a budgeted (compressed) size rather than the full raw ceiling,
  growing only as individual frames need it. Measured live at 30s@1080p: ~2.9GB with the buffer
  active, down from ~6.3GB pre-compression.
- Windows + Linux only for now — macOS has no equivalent trusted static FFmpeg build available and
  keeps working exactly as in v0.2.0 (uncompressed).
- Still open: measuring the real compression ratio against sustained gameplay content (today's
  only data point came from a static loading scene, not representative); a slot's RAM growth is
  permanent for the session once it happens (never shrinks back until Disable()).
- `main` is now a protected branch (PR required, even for the maintainer); `develop` is where
  ongoing work happens.

## v0.4.0 (shipped — 2026-09-07)
- **Report a problem**: a native dock dialog that creates a real trigglow.com support ticket
  (category `dynamic_delay`) and attaches the current OBS log automatically — no manual log
  hunting for the user. New `HttpsPostMultipartFile()` on the existing WinHTTP client
  (`src/win-http.cpp`) for the attachment upload.
- **In-app update check**: on load, compares this repo's latest GitHub release against the running
  version (public, unauthenticated) and shows a dismissible-by-design dock notice if newer —
  silent on failure or when already current, never an error.
- **Real localization**: every user-facing string (dock, report-a-problem dialog, filter names,
  hotkey descriptions, buffer status messages) now goes through OBS's own `obs_module_text()`
  locale system instead of hardcoded Spanish literals — `data/locale/en-US.ini`/`es-ES.ini` are
  now fully translated rather than holding a single `PluginName` key each. Previously a user
  running OBS in any language other than Spanish still saw a 100% Spanish plugin.
- **Dock redesign**: card-grouped sections, a consistent semantic status-color palette, and
  side-by-side delay/quality controls.
- **RAM measured for the first time, not assumed**: a `compression check` log line
  (`src/video-delay-filter.cpp`) exposed a real ~11-15x MJPEG compression ratio on live gameplay,
  well above the 3x conservatively assumed since v0.3.0 — `kAssumedCompressionRatio` retuned to
  5.0 on that data, plus removed a per-tick encode-frame allocation. Measured live at 30s/1080p60:
  total OBS process memory went from ~2.8GB to ~2.1GB.

## v0.5.0 (shipped — 2026-10-04)
- **"Delay Ns" overlay** (logo badge + text, 40% size) shown over the delayed output while
  Active, with an on/off checkbox and a four-corner selector in the dock.
- **Single Enable/Disable button** and a **responsive dock** (scroll area + rows that stack when
  narrow).
- **Video delay by wall clock.** A production log showed the frame-count ring running 48-90
  slots/s at 60 fps (delay really ~20-38s vs audio exactly 30s). Capture is now one slot per
  1/fps cell and playback picks the slot by capture time; audio also follows timestamps.
- **Wrapper scene always holds the selected live scene**, and delay filters left enabled by an
  OBS close while Active are switched off on load.
- **Windows installer packaging fix:**
- **The Windows installer never shipped `es-ES.ini`.** Found live testing on an OBS install set to
  Spanish: the plugin appeared stuck in English regardless of OBS's language setting.
  `installers/windows/trigglow-dynamic-delay-setup.iss`'s `[Files]` section only ever listed
  `en-US.ini` — a packaging oversight predating this session, invisible since v0.4.0 first made
  `es-ES.ini` a real translation (before that it held a single placeholder key, so a missing file
  changed nothing user-visible). `obs_module_text()` falls back to the default locale whenever the
  requested language's `.ini` isn't present on disk at all, which looks identical to "this plugin
  doesn't support Spanish" from the user's side. Now ships both. This is the next release after
  v0.4.0 (currently in production) — small enough on its own that it doesn't need to wait on the
  unrelated features below.

## v0.6.0 (shipped — 2026-10-19)
Performance, adaptivity and the Trigglow look, plus Trigi, a safer sign-in and a security pass. A
v0.5.1 performance hotfix was prepared but never published; all of it ships here. Reported live
first: "Encoding overloaded" as soon as the delay was enabled with a 2560x1440 canvas and a
1920x1080 output at 60 fps.
- **Buffer capped at OBS's output resolution.** The ring was sized from the canvas, so a 1440p
  canvas streamed at 1080p was MJPEG-encoded/decoded at 1440p on OBS's graphics thread
  (14.3/17.4 ms p50/p95 per frame). The minimum quality became a RAM floor that never exceeds the
  output. Sizing math in `src/buffer-fit.*`, unit-tested; widths multiples of 8 for OpenGL readback.
  Hidden `buffer_output_cap` support setting, and a log warning for encoders above the buffer.
- **Encode/decode on worker threads, off OBS's graphics thread — 1440p60 output supported.** One or
  two encode workers, one decode worker that decodes ahead, a reaper thread that frees memory, a
  ring of slot headers with a state machine and a block arena. At 1440p60 the plugin's work on the
  graphics thread went from ~14/17 ms to ~1.2–1.7/1.6–2.1 ms per frame (p50/p95, offline
  microbenchmark). Hidden `codec_workers` kill switch back to the codec on the graphics thread.
- **Adaptive degradation that never cuts the delay**: more encode threads, then lower resolution
  rungs down to the minimum quality, then half the capture rate; back up with 30 s of hysteresis;
  amber notice in the dock saying what was lowered and why. The minimum quality goes up to 1440p
  (2K) and 2160p (4K).
- **Less RAM, nothing heavy on the graphics thread**: no zero fill, arena by blocks (~1.9 GB →
  ~0.6–0.8 GB for 30 s at 1440p60 in simulation), memory freed on the reaper, codecs and GPU objects
  freed on Disable, I420 conversion on the GPU, standard Huffman tables, each frame decoded and
  uploaded once.
- **One whole-second delay for video and audio**, also when RAM trims the ring (was up to ~1 s
  apart); exact frame interval at 59.94 fps; a buffer restart refills behind the loading scene
  instead of showing black; 0 s passes the video through.
- **Delay filters left on by an OBS close while Active are really switched off**, at startup and
  on a scene collection switch; filter reference leak and destructor without graphics context
  fixed; an unreadable settings file is set aside as `.invalid`.
- **`perf check` and `pipeline check` telemetry** every 10 s in the OBS log (replaces
  `compression check`).
- **UI redesign aligned with trigglow.com**: dock, report dialog and live overlay on top of the OBS
  theme, bundled Archivo and Geist Mono (OFL), SVG icons, WCAG AA contrast and keyboard focus,
  sign-in errors shown, the delay seconds locked while the delay runs. The overlay is one generated
  image, the same on every OS, signed "By Trigglow", that keeps the position and size you give it.
- **"(auto)" names** for the scene, filters and overlay the plugin manages, migrated automatically.
- **Trigi, Trigglow's AI support assistant, in "Report a problem"** (Windows, signed in): first-use
  AI and privacy notice, optional OBS technical data (numbers and states only, with a preview), the
  OBS log never sent to the AI, every error state with its next step, safe rendering of answers,
  and the conversation turned into a ticket with a summary.
- **Sign-in v2**: a code to compare on the trigglow.com page with "It wasn't me", the plugin's own
  scoped sign-in encrypted with the Windows data protection API, renewed automatically and revoked
  by Sign out (or from trigglow.com's connected apps), and a one-time automatic move from v0.5.x.
  The v0.5.x-and-earlier sign-in is switched off when this version is published, so updating is
  required.
- **Signed-in tickets** with an optional subject and a version footer; the OBS log redacted in
  memory, previewed and attached only with consent; old copies in `%TEMP%` deleted; guests sent to
  the web form when trigglow.com asks for a human check.
- **Hardening**: one WinHTTP path (no redirects, TLS 1.2+, timeouts, size cap, the token only for
  api.trigglow.com), fixed pages only, no codes, URLs or user paths in the OBS log, the update notice
  limited to this repo's releases, no build paths in the DLL, and every network call cancellable so
  OBS closes cleanly even while Trigi is answering.
- **Build/CI**: FFmpeg re-pinned to a BtbN month-end build, 19 unit test suites in CI on every
  platform, a ThreadSanitizer job, `Qt6::Svg`.

## v0.7.0 (shipped — 2026-10-20)
An optimization release: the delay buffer takes about half the RAM on Windows. The planned v0.6.1
(HTTP client for macOS and Linux) wasn't published; it's next.
- **Hardware storage codec on Windows.** The buffer is stored as HEVC (H.264 on a GPU without HEVC)
  with the GPU's encoder (NVENC, AMF, Quick Sync) and decoded with D3D11VA, instead of MJPEG:
  1440p60 from ~675 MB to ~325 MB for 30 s and from ~1.2 GB to ~520 MB for 60 s, PSNR-Y of the
  stored frames from ~40 to ~45 dB, codec CPU from ~0.8 to ~0.2–0.3 cores (offline, AMD AMF). MJPEG
  stays as the bridge while an encoder is set up and as the fallback. No new DLLs.
- **A ring that takes inter-frame codecs**: per-packet codec, key, chain and seq, a GOP of slack,
  decode from a key with catch-up, several codecs in one ring, an asynchronous encoder interface.
- **"Encoding overloaded" guard** for a GPU engine shared with OBS's encoders: one rung down on an
  early latency warning (5-minute veto), back to MJPEG at once on a frame OBS skips for encoding
  lag, an OBS encoder error or any hardware error, and the NVENC session handed to an output
  starting.
- **Sizing**: storage class fixed per cycle (12x once a hardware codec has worked in the session,
  2.4 times the frames when RAM is short; MJPEG's 5x on the session's first cycle), the buffer
  capped at the stream encoders' resolution when they all rescale below the output, 8 MiB arena
  blocks with one spare, at most two slice threads per MJPEG context.
- **Support**: hidden `buffer_codec` setting (`"auto"` or `"mjpeg"`), `codec=` in `pipeline check`,
  a `storage codec check` line, `PipelineHealth` v2. No new UI, texts or RAM warnings.
- **Security and licensing**: no Quick Sync in an elevated OBS, `get_pipeline_state` bounded by the
  caller's struct size, `LICENSE-ffmpeg.txt` shipped and an LGPL check of the FFmpeg build in CI.
- **Tests**: 23 suites in CI (fake inter-frame codec stress and adversarial tests, the codec
  policy), plus the hardware codec over the real FFmpeg on local Windows builds.

## v0.7.1 – v0.7.3 (shipped — 2026-10-20 to 2026-11-11)
- **0.7.1**: OBS 33's plugin locations (`C:\ProgramData\obs-studio\plugins` on Windows), a check
  of the minimum OBS version (31.1.0), Linux packages for Ubuntu, Fedora and Arch.
- **0.7.2**: the plugin's core for Trigglow Studio (`-DTRIGGLOW_CORE_ONLY=ON`), the update check on
  the repository's new address.
- **0.7.3**: the global audio sources and the audio inside nested scenes or groups delayed too.

Details in `CHANGELOG.md`.

## v1.0.0 (2026-11-11) — out of beta
The first stable release on Windows. macOS and Linux stay alpha (built and tested in CI, not run on
a live stream yet).
- **Stream mode (Windows)**: the output-level delay that was listed under "Later" since 2026-09-08.
  The plugin points OBS's stream output at a relay on `127.0.0.1` and sends the already-encoded
  stream to the platform after the delay: no decode, no second encode, RAM = bitrate × seconds kept.
  0–600 s changed live with no reconnect, every scene delayed, recording / Replay Buffer / virtual
  camera live, multistream from one encode, drain on stop. Details: `docs/STREAM_MODE.md`.
- **Dump what viewers haven't seen**, **delay by scene** (on with its own seconds, or off, per
  scene), **three presets** with hotkeys, the **delay badge** in stream mode (up to 600 s) and
  **settings checks** (keyframe interval, OBS's own Stream Delay). Stream mode only, except presets.
- **Hotkeys**: Back to live, Delay +5 s / -5 s, Dump, Preset 1–3, next to the original three
  (same names and bindings).
- **obs-websocket control**: vendor `trigglow-dynamic-delay` with `GetStatus`, `EnableDelay`,
  `DisableDelay`, `ToggleDelay`, `SkipToLive`, `SetDelay`, `AdjustDelay`, `DumpDelay`,
  `ApplyPreset` and the `StatusChanged` event, for Trigglow's Stream Deck plugin.
- **Sign-in on macOS and Linux** (the HTTP client planned as v0.6.1): libcurl with the WinHTTP
  rules (HTTPS on 443 only, TLS 1.2+, no redirects, capped body), the sign-in in a user-only
  (0600) file. With it, turning the delay on, Trigi and "Report a problem" work there too.
- **Trigglow's new design** in the dock, a new overlay plate, the new Dynamic Delay logo, the
  default "Adding delay" loading video, no more "Beta" tag.
- **Translations**: Portuguese (Brazil) and Chinese (Simplified), next to English and Spanish.

## Next
- **Run stream mode live on real platforms** before calling it verified: Twitch (RTMP), Kick
  (RTMPS), YouTube (HEVC over enhanced RTMP), with delay changes, a dump and an OBS stop mid-delay,
  watching the platform's player. So far it is unit-tested and checked with the local relay harness
  only (`docs/STREAM_MODE.md`, "Verification status").
- **Stream mode on macOS and Linux.** The plugin's FFmpeg there is built without avformat (and
  without a TLS stack for RTMPS), so those builds only have scene mode. Needs avformat with RTMP
  and TLS in the Linux FFmpeg build and an FFmpeg for macOS, then the same relay.
- **Enhanced Broadcasting, SRT, RIST and WHIP in stream mode.** Today those streams fall back to
  scene mode: Enhanced Broadcasting is several tracks on one multitrack output, and SRT, RIST and
  WHIP aren't RTMP, so the relay would need another sender for each.
- **Code signing** (CA-issued, Windows and macOS): the real fix for the SmartScreen warning and
  the Defender false positive in
  [#10](https://github.com/TrigglowStudios/trigglow-dynamic-delay/issues/10). The application to
  [SignPath Foundation](https://signpath.org/apply)'s free program for open-source projects is in
  progress. Until then the Windows installer is signed with a self-signed certificate
  (`.github/workflows/build-project.yaml`, "Sign Windows Installer (self-signed)"), which only
  proves CI produced it: it removes neither the "Unknown publisher" warning nor the false positive.
  Keep that step as a fallback once a real certificate lands.
- **Trigglow's Stream Deck plugin with Dynamic Delay keys** (toggle, back to live, presets, +/-,
  the delay's state on the key) over the obs-websocket vendor; the native-hotkey path stays as the
  dependency-free option.
- **macOS/Linux live verification** of scene mode and the new sign-in: both build green in CI;
  this is about running and confirming them on those platforms.
- Scene mode:
  - **Validate the hardware storage codec on NVIDIA inside a real OBS**: NVENC shared with the
    stream's encoder (and Enhanced Broadcasting) with zero frames skipped for encoding lag, NVDEC,
    the VRAM it takes and the bytes per frame against the AMD figures. Quick Sync too, on Intel
    hardware.
  - **Zero copy on the GPU.** Encode straight from OBS's texture and decode into a D3D11 texture,
    so the CPU copies of each frame go away: by our estimate another 50–120 MB less and less work
    on OBS's graphics thread. Riskier (sharing OBS's D3D11 device with FFmpeg across threads).
  - **Deduplicate captures by OBS's frame time** (`obs_get_video_frame_time()`) instead of a
    wall-clock cell per frame interval, so render jitter can't put two renders in one cell and lose
    a real frame.
  - **A smaller keep-alive render target** while the buffer fills (today a canvas-sized texture
    whose content is never used: 14.7 MB at 1440p).
  - Verify 1440p60 output and the degradation inside a real OBS session (the figures come from
    offline benchmarks on an AMD CPU and GPU), and the A/V alignment with a clock and a clap.
  - Revisit the assumed compression ratios (5x for MJPEG, 12x for the hardware codec) with the
    `ratio` and arena figures of the `pipeline check` line from more sessions and games.

## True-live Replay Buffer (solved by stream mode on Windows; scene mode unchanged)
Stream mode never touches OBS's program, so the Replay Buffer, the recording and the virtual camera
keep the live picture while the stream is delayed. What follows is the 2026-09-08 study for **scene
mode**, still paused: there the delay filters sit on the live scene's own sources, so any other
output sees the delayed picture while the delay is on.

- **First-pass idea, and why it's wrong.** This entry originally proposed the
  `exeldro/obs-source-record` "Branch Output" pattern verbatim: a dedicated `obs_view` +
  encoder + `"replay_buffer"` output (all public OBS API — `obs_view_create`/`obs_view_add2`,
  `obs_video_encoder_create`, `obs_output_create("replay_buffer", ...)`, source read from Source
  Record's actual implementation) pointed at the real live scene, running alongside the streaming
  output that keeps showing the delayed wrapper. That's wrong for THIS plugin specifically: filters
  are a property of the source object itself, not of whoever's rendering/reading it — verified
  directly against `libobs/obs-source.c` (`obs_source_output_audio()` runs the full filter chain via
  `filter_async_audio()` *before* `source_signal_audio_data()` reaches ANY capture callback or
  consumer; `obs_source_default_render()` works the same way for video). Since `VideoDelayFilter`/
  `AudioDelayFilter` are attached directly to the live scene's/leaf sources' own objects (nesting
  never duplicates them — see `EnsureBufferWrapperScene`'s header comment), a brand-new `obs_view` or
  encoder pointed at "the real live scene" would see the SAME already-delayed output the streaming
  side does, the moment buffer mode goes Active — exactly the state where a true-live replay would
  matter most. Source Record itself never hits this, since its whole point is capturing a source
  *with* whatever filters it has.
- **What actually fixing this would take.** `VideoDelayFilter`/`AudioDelayFilter` already capture an
  undelayed copy of every frame/audio buffer each tick, before writing it into their delay ring — the
  correct fix is having them ALSO push that same undelayed copy into a dedicated `video_output_t`/
  `audio_output_t` (`video_output_open()`/`video_output_lock_frame()`, mirrored for audio) that a new
  replay-buffer encoder+output reads from, instead of building anything as a separate, isolated
  addition. That means changing the internals of the two files this plugin's core delay guarantee
  depends on — not a self-contained new feature next to them.
- **Why paused rather than attempted now.** `docs/SPEC.md`'s own history (§3.3's v0.3.1/v0.3.2
  entries, §7's unresolved GPU crash) shows this exact pair of files has repeatedly surfaced subtle,
  only-found-live bugs even from focused, delay-specific changes. With stream mode covering the
  need on Windows, this only matters for macOS and Linux until stream mode reaches them.

## Out of scope for now
- **Following the streamer's scene switches while delayed in scene mode, without a refill gap**
  (investigated and rejected, 2026-09-08). Scene mode's filters only ever buffer ONE scene's
  frames, so a switch to another scene would have to fill again from empty, which breaks the
  "never cuts" promise; buffering every scene in advance would multiply the RAM and GPU cost by the
  scene count. Stream mode solves it (it delays the composited, encoded stream, so a scene switch
  is part of one continuous signal) and delay by scene covers "a different delay per scene" there.
- External web panel as the main way to control it (product decision: everything lives natively
  in OBS).
