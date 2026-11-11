*[Versión en español](FAQ-es.md)*

# FAQ — Trigglow Dynamic Delay

## Stream mode (v1.0.0, Windows)

**What's the difference between stream mode and scene mode?**
Stream mode (new in 1.0.0, the default on Windows) delays the stream OBS has **already encoded**:
the plugin points OBS's stream output at a relay inside OBS, on `127.0.0.1`, keeps the packets in
RAM and sends them to your platform after the delay. Scene mode (the only mode before 1.0.0, now
"Scene mode (compatibility)" in the dock) keeps your rendered live scene in RAM and plays it back in
a helper scene, before OBS encodes it. Stream mode costs far less RAM and CPU, delays every scene,
leaves your recording live and changes the delay while you're live; scene mode works with any
output and on every platform. Details: `docs/STREAM_MODE.md`.

**Why does stream mode use so little RAM?**
It keeps what OBS's own encoder already produced for the stream, and nothing else: nothing is
decoded or encoded again, the CPU only copies bytes. The RAM is your bitrate times the seconds kept:
about 45 MB per minute at 6 Mbps. By default it keeps 120 s ("Keep for instant raises"), so the
delay can be raised up to that live with no wait: about 90 MB at 6 Mbps. Scene mode stores frames
at a quality close to your source, which is why it takes ~190 MB to ~2.3 GB for 30–60 s.

**Which streams can use stream mode?**
Streams that go out through OBS's own RTMP output over RTMP or RTMPS: Twitch, YouTube, Kick, TikTok
and any custom rtmp:// or rtmps:// server, with Enhanced Broadcasting off. Not (yet): Enhanced
Broadcasting, SRT, RIST and WHIP services, and macOS and Linux (their FFmpeg doesn't have the
part the relay needs yet). In those cases the dock says why and scene mode works as before.

**Does the stream reconnect when I turn the delay on, off, up or down?**
No. The relay keeps one connection to your platform and splices: turning the delay on (or raising
it) jumps back to an older keyframe, so viewers see the last seconds again, never anything newer;
turning it off (or lowering it) jumps forward. Timestamps are rewritten so the platform sees one
continuous stream. If you turn the delay on before you start streaming, the stream starts that many
seconds late on the platform.

**Why does the dock ask me to set the keyframe interval to 2 s?**
The delay can only move from keyframe to keyframe, so the keyframe interval is the step it moves
in. With "auto" (0) most encoders use 4–10 s. The dock warns when it's on auto or above 4 s; set it
in `Settings → Output` (Advanced output mode, "Keyframe Interval"). 2 s is also what most platforms
ask for.

**The dock warns that OBS's own Stream Delay is on. Why?**
`Settings → Advanced → Stream Delay` adds its fixed delay on top of this one. Turn it off and use
Dynamic Delay's.

**What does "Dump what viewers haven't seen" do?**
Something slipped on screen while delayed? Press it (dock, hotkey or obs-websocket): the seconds
still waiting in the delay are never sent. The stream jumps to live within about one keyframe
interval, and the delay comes back on its own after its seconds plus 3 s, rewinding only into what
came after the dump. It does nothing when the delay is off or in scene mode.

**What are presets and "Delay by scene"?**
Three presets (10, 30 and 60 s by default) each set the seconds and turn the delay on, from the
dock, a hotkey or obs-websocket. "Delay by scene" turns the delay on (with its own seconds, or the
current ones) or off when you switch to a scene: on in your game scene, off in Just Chatting. Rules
apply when OBS switches scene and when a stream starts, in stream mode only.

**What happens when I stop streaming while delayed?**
By default the plugin keeps sending the seconds viewers haven't seen yet, then stops ("Stream
stopped: sending the last 30 s"). **Stop sending now** cuts it, and you can turn it off with
"When I stop streaming, send the seconds viewers haven't seen". Starting a new stream or closing OBS
cuts it too.

**Why does OBS show 0 dropped frames in stream mode?**
OBS now sends the stream to the relay on your own computer, so its own congestion counter only
sees that local link. The dock shows the connection's state and rate instead, and if the
stream falls more than 4 s behind (slow upload) jumps back to the delay at the next keyframe rather
than letting the delay grow.

**Is stream mode tested?**
Its core (the splicing, timestamps, trimming and dump) is unit-tested, and the relay was checked
end to end on a local machine with a test stream, including delay changes and a dump. It has **not
yet been run on a live stream to a real platform** (Twitch, Kick, YouTube); that's the next step
before it's called verified (`docs/STREAM_MODE.md`, "Verification status"). Scene mode stays
available if anything goes wrong.

## General

**Does the stream cut out or reconnect when I enable, disable, or use the delay?**
No — never, at any point, for any reason, in either mode. Stream mode keeps one connection to your
platform and splices the delayed stream (see above). Scene mode never calls into OBS's own
streaming-output delay setting at all. Instead, two OBS filters buffer video and
audio in a system-RAM ring, and the plugin switches OBS's Program between your live scene, an
optional loading scene, and a delayed "wrapper" scene entirely internally — the streaming output
itself never sees any of that happen. Full technical detail, including why an earlier
reconnect-based design was tried and abandoned, in `docs/SPEC.md` §2–§3.

**Is the audio delayed too, in sync with the video?**
Yes. Video and audio use the same configured delay in seconds, so they land back in sync — audio
uses a separate filter attached to each individual audio-capable source inside your live scene
(OBS doesn't allow an audio filter to attach directly to a scene itself), but both run off the same
`delaySeconds` value. As of v0.3.2, this stays true even if the RAM budget forces video to actually
buffer less than the full requested duration (see the next question) — audio automatically matches
whatever video is really able to deliver instead of drifting ahead. Since v0.6.0 both use exactly
the same delay in whole seconds in that case too (before, they could end up to ~1 s apart), and the
dock and the live overlay show that same figure. See `docs/SPEC.md` §3.2/§3.3.

**How do the "Delay (seconds)" and "Minimum quality" settings interact?**
Independently, by design — you pick both freely. The minimum quality (480p to 2160p (4K)) is how far
the plugin may lower the buffer's resolution to save RAM: it never goes below it, and shortens the
actual buffered duration instead if your requested seconds don't fit the RAM budget detected on
your machine at that quality. Since v0.6.0 it never raises the buffer above OBS's output resolution,
though (Settings → Video → Output (Scaled) Resolution): with a 720p output the buffer is 720p even
if you pick 1080p, because OBS has nothing above its output to send. Since v0.7.0, when every
encoder that sees your canvas (stream, recording, Enhanced Broadcasting tracks) encodes below the
output, the buffer is stored at the largest of them: a 1440p output streamed by an encoder rescaled
to 1080p, with nothing recorded above 1080p, buffers at 1080p. It's also the floor for the
automatic quality reduction when your computer can't keep up (see below). The dock shows a live,
non-blocking estimate of what a given combination will actually achieve (seconds and resolution) as
you adjust either setting, before you ever press Enable. See `docs/SPEC.md` §3.5.

**What happens if my requested delay + quality doesn't fit in RAM?**
You get a clear warning in the dock *before* enabling — it never fails silently and never crashes.
The plugin simply buffers fewer seconds than requested at your chosen quality rather than ever
dropping below it; lowering the seconds or the quality floor gets you back to the full requested
duration.

**Can I change the delay while it's on?**
Yes, since v1.0.0, in both modes. In stream mode the new delay applies at the next keyframe. In
scene mode, lowering it skips viewers forward at once, and raising it holds the picture (or shows
the loading scene above 3 s) only while the extra seconds build up; the buffer is never emptied.
Typed values apply on Enter or when the field loses focus, so "300" never passes through 3 and 30.
The scenes and the minimum quality stay locked while scene mode's delay runs: turn the delay off to
change them.

**What's the maximum delay it supports?**
600 seconds (10 minutes) in both modes. Stream mode keeps the already-encoded stream, about 45 MB
per minute at 6 Mbps, so 600 s fits almost anywhere. In scene mode the real ceiling is whatever fits
your machine's detected RAM budget (roughly half of total system RAM, floored at 1GB and capped at
24GB) at your chosen quality — if the seconds at your chosen quality don't fit, the dock's live
estimate will tell you before you enable it, and the plugin buffers as many seconds as actually fit
instead.

**Does it compress the buffer? Does that lower quality?**
Yes. It only changes how the buffered frames are kept in memory: the quality floor you pick (480p to
2160p (4K)) works exactly the same way.
- **Windows with a hardware video encoder (v0.7.0):** the frames are stored as HEVC video (or H.264
  on a GPU without HEVC) with your GPU's encoder (NVIDIA NVENC, AMD AMF or Intel Quick Sync), and
  decoded on the GPU. A key frame every half second and the frames in between stored as differences,
  which is why it takes about half the RAM of MJPEG. It's also closer to the original: in our
  measurements the stored frames come back at a PSNR-Y of ~45 dB, against ~40 dB with MJPEG.
- **Windows without a hardware encoder, and Linux:** each frame is MJPEG-compressed on its own, as
  since v0.3.0, visually close to lossless. Real gameplay compresses about 11–15x, though it depends
  on what's on screen.
- **macOS:** the buffer is stored uncompressed.

Compression and decompression run on the plugin's own threads (and on the GPU's video engine with
the hardware encoder), not on OBS's graphics thread, and if a frame can't be compressed it's stored
uncompressed instead. See `docs/SPEC.md` §4 and §4.8.

**How much RAM does the delay buffer take?**
It depends on the output resolution, the delay, what's on screen and how it's stored. Measured
outside OBS with gameplay-like content at 60 fps, the buffer alone:

| Output | 30 s, hardware HEVC (Windows, v0.7.0) | 30 s, MJPEG | 60 s, hardware HEVC | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1,200 MB |
| 2160p60 | ~590 MB | ~1,250 MB | ~920 MB | ~2,300 MB |

- The hardware column was measured with an AMD GPU (AMF). **NVIDIA's NVENC isn't measured yet**;
  its bytes per frame, and so the RAM, may differ somewhat.
- Against MJPEG, the hardware codec takes 49–62% less depending on resolution and delay. v0.6.0
  took a little more than the MJPEG column: about 675 MB at 1440p60 for 30 s, ~1.2 GB for 60 s.
- Add about 11 MB per audio source with sound for 30 s of delay (22 MB for 60 s).
- Content that changes a lot every frame compresses worse, with both codecs.
- To see what your buffer really takes, look for `arena=…MB committed` in the `pipeline check` lines
  of the OBS log: that's the stored frames, the biggest part (the codecs and frame pools add about
  50 MB with MJPEG and 110 MB with the hardware codec at 1440p). `codec=` in the same line says how
  it's stored (`docs/TROUBLESHOOTING.md`, "Reading the log lines").

**Does the buffer use my GPU's video encoder? Can that cause "Encoding overloaded"?**
On Windows, yes: since v0.7.0 the buffer uses the GPU's hardware encoder when there is one, and the
GPU's decoder too. It's built so that your stream's encoder always comes first. When the buffer
encodes on the same engine as one of OBS's encoders (both on NVENC, for example):
- if that engine starts to get busy (the buffer encoder's latency rises), the buffer drops one
  resolution step, never below your minimum quality, and doesn't go back up for 5 minutes; the dock
  shows its amber "Buffer quality lowered" notice;
- if OBS skips even one frame for encoding lag, or one of its outputs stops on an encoder error, the
  buffer goes back to MJPEG at once, which doesn't use the engine at all. The delay doesn't cut or
  move; the buffer just takes more RAM until you turn the delay off;
- when OBS starts streaming, recording or the Replay Buffer, the buffer gives its NVENC session to
  that output first and takes it back a few seconds later.

If the buffer encodes on another engine (an iGPU while you stream on the dedicated GPU, or a stream
on x264), it can't cause encoding lag, and none of this needs to kick in. This guard was tested with
an AMD GPU; **NVENC hasn't been validated inside a real OBS yet**. If you suspect the buffer, force
MJPEG (next question) and compare.

**Can I make the buffer use MJPEG, like before v0.7.0?**
Yes, with the hidden `buffer_codec` setting: `"buffer_codec": "mjpeg"` in the plugin's
`buffer-mode-settings.json`, edited with OBS closed. It has to be exactly that lowercase string;
anything else keeps the default, `"auto"` (hardware where it works, MJPEG otherwise), and the log
says why. It's meant for support or to rule the hardware codec in or out of a problem: MJPEG takes
about twice the RAM and more CPU. Steps in `docs/TROUBLESHOOTING.md`, "Hidden settings for support".

**OBS shows "Encoding overloaded" as soon as I enable the delay. Why?**
Up to v0.5.0, compressing and decompressing the buffered frames ran inside OBS's graphics thread,
which has 16.7 ms per frame at 60 fps, and the buffer was stored at your canvas resolution: with a
canvas larger than the output (for example 2560x1440 canvas, 1920x1080 output) that work took most
of the frame and OBS fell behind. **v0.6.0 fixes it**: the buffer is never stored above OBS's output
resolution, and compression runs on the plugin's own threads, so what's left on OBS's graphics
thread is a millisecond or two per frame. Update to the latest version; if you lowered your canvas
to work around it, you can set it back. Since v0.7.0 the buffer may also use the GPU's video
encoder, but it gives way to OBS's encoder (previous questions). If it still happens, see the
"Encoding overloaded" section of `docs/TROUBLESHOOTING.md`.

**Does it only work at 1080p? What about 2K (1440p)?**
It works at any resolution — the buffer follows OBS's output resolution. What matters for
performance is the *output* resolution and frame rate, not the canvas:
- **Canvas larger than the output** (e.g. 1440p canvas, 1080p output): fine since v0.6.0, same cost
  as a 1080p output.
- **1440p output at 30 or 60 fps:** supported since v0.6.0. In our offline benchmark at 1440p60 (an
  8-core/16-thread CPU) the plugin's work on OBS's graphics thread is ~1.2–1.7/1.6–2.1 ms p50/p95 per
  frame, and compression and decompression keep under one CPU core busy in total on their own
  threads.
- **Minimum quality 1440p (2K) or 2160p (4K):** in the dock since v0.6.0. With a 1440p output and
  a 1440p minimum the buffer never drops below 1440p, neither to save RAM nor when your computer
  can't keep up (then only the capture frame rate halves, with an output at 48 fps or more): if
  the seconds don't fit in RAM, the
  delay gets shorter instead, as with 1080p. Above your output it changes nothing: with a 1080p
  output the buffer stays 1080p. To see the RAM the buffer really takes, look for
  `arena=…MB committed` in the `pipeline check` lines of the OBS log (every 10 s while the delay
  runs): at 1440p60 with 30 s and gameplay, expect roughly 200–250 MB with the hardware codec
  (`ratio=` around 40–60x) and 600–900 MB with MJPEG (around 11–15x); the codecs and frame pools
  add about 50 MB (MJPEG) to 110 MB (hardware) on top. The plugin plans for up to ~1.9 GB there with
  MJPEG's assumed 5x, or ~0.8 GB with the hardware codec's 12x once it has worked in this OBS
  session, and the dock warns before you enable it if that doesn't fit.
- **If your computer can't keep up** at your settings, the plugin lowers the buffer's resolution
  (never below your minimum quality) and, as a last step, its capture frame rate, instead of
  letting OBS fall behind or cutting the delay. The dock tells you with an amber notice (next
  question).

**What does the amber "Buffer quality lowered" notice in the dock mean?**
Your computer couldn't keep up with the delay buffer at full quality, so the plugin lightened it:
first it adds compression threads (no notice for that), then it buffers at a lower resolution, never
below your minimum quality, and as a last step it captures every other frame (only with an output at
48 fps or more). Since v0.7.0 it also lowers one step when the GPU video engine the buffer shares
with OBS's encoder gets busy (the notice then names compressing the frames as the cause, and it
doesn't go back up for 5 minutes). The notice says what was lowered and why. The delay in seconds
never changes and the stream never cuts; repeated frames, if any, show as a brief stutter. When the
load drops, it goes back up by itself after 30 s with room to spare, and the notice disappears.
What to do about it, if anything: `docs/TROUBLESHOOTING.md`.

**Does it affect local recording or the Replay Buffer?**
**In stream mode, no**: recording, the Replay Buffer and the virtual camera keep the live picture
while viewers get the delayed stream. The one thing they share is the delay badge, if you show it:
it's drawn over OBS's program, so the recording shows it too while the delay is on.

**In scene mode, indirectly yes**, and this is worth understanding: buffer mode works by switching what OBS's Program
is showing, and both local recording and the Replay Buffer capture that same Program output — so if
you're recording (or have the Replay Buffer running) while the delay is `Active`, what gets saved is
the delayed content too, same as what viewers see. There's currently no way to delay the stream
without also delaying recording/Replay Buffer in scene mode; use stream mode for that.

**The plugin's scene and filters have new names. Do I need to do anything?**
Since v0.6.0 they end in "(auto)" instead of "(no tocar)": **Trigglow Delay Buffer (auto)**,
**Trigglow Buffer Mode Delay (auto)**, `Trigglow Audio Delay (auto) - <source>` and **Trigglow Delay
Badge (auto)**. Your scene collections are renamed automatically. Only update Stream Deck,
obs-websocket or other remote-control actions that use those objects **by name**; the plugin's
hotkeys aren't affected. If you go back to v0.5.0 or earlier afterwards, it creates its own
"(no tocar)" set next to the "(auto)" one: delete the set you don't use.

**What is the badge viewers see during the delay?**
One image the plugin draws itself: since v1.0.0 a rounded plate with the Dynamic Delay logo,
"DELAY" and your seconds, and "Dynamic Delay by Trigglow" under them, the same in every language. It
only shows while the delay is on. In scene mode it's the source **Trigglow Delay Badge (auto)**:
**Show delay overlay on stream** in the dock turns it off, the dock picks the corner, and you can
move and resize it in OBS (it stays where you put it). In stream mode it's drawn over OBS's
program on its own output channel (up to 600 s): **Show the delay badge to viewers** and the corner
are in the stream mode section, and it can't be moved by hand.

**What triggers the `Error` state?**
Anything that would otherwise leave the plugin in a broken but silent condition: no live scene
chosen, or a chosen live scene that no longer resolves to anything (e.g. it was deleted). The dock
shows a clear message explaining what's wrong instead of hanging or crashing.

**Does it work with Twitch/YouTube/Kick/any platform?**
Yes. Scene mode works entirely on OBS's own Program output, so the platform doesn't matter. Stream
mode works with any platform OBS streams to over RTMP or RTMPS (Twitch, YouTube, Kick, TikTok,
custom servers); SRT and WHIP services use scene mode.

**Do I need a web panel or a Trigglow account to use it?**
No web panel: all control lives inside OBS. Since v0.3.3 you do need to sign in once to a free
trigglow.com account before the delay can be turned on, in either mode (the sign-in happens in your
browser, and the plugin stays 100% free) — see `docs/ACCOUNT_GATE.md`. Since v1.0.0 sign-in (and
with it the delay and Trigi) works on macOS and Linux too, not only on Windows.

**How does signing in work, and what does the plugin keep?**
Signing in always happens on trigglow.com, in your browser: the plugin never asks for your password.
Press **Sign in** in the dock; it shows a code ("The page must show the code KD7F-29QM."), and the
trigglow.com page shows the same code with the app, its version and when it was requested. Confirm
only if the codes match and you started it; **It wasn't me** cancels it. From then on the plugin
keeps its own sign-in, which only works for what the plugin does (checking your account, Trigi and
your own Dynamic Delay tickets). On Windows it's encrypted with Windows' data protection for your
Windows user; on macOS and Linux it's kept in a file only your user can read (mode 0600), not
encrypted, the way OBS keeps your stream key. Your email isn't stored. **Sign out** in the dock also disconnects it on trigglow.com, and you can
disconnect it from the web too: Settings → Active sessions → Connected apps.

**I'm on v0.5.x or earlier. Do I have to update?**
Yes. The sign-in of v0.5.x and earlier is switched off when v0.6.0 is published: from then on
those versions can no longer sign in to Trigglow, and the sign-ins they already have stop working
after a transition period, so the delay stops working there. Install the latest version over your
current one, with OBS closed. On the first start your sign-in moves to the new format by itself; if
the dock asks you to sign in, do it once. Your scenes and settings are kept.

**The dock or "Report a problem" says to update the plugin. What do I do?**
"This plugin version can't sign in any more. Update the plugin." in the dock, "Update the plugin to
use Trigi" in "Report a problem" or "Update required" on the trigglow.com sign-in page all mean the
same: your version is too old for what you're trying to do. Download the latest version (**Get the
update** in the dialog, the update notice in the dock, or the
[latest release](https://github.com/TrigglowStudios/trigglow-dynamic-delay/releases/latest)) and install
it with OBS closed; your settings and your sign-in are kept. Meanwhile you can still send a report
from the form in the dialog or from [trigglow.com/support](https://www.trigglow.com/support).

**What is Trigi?**
Trigglow's AI support assistant, the same one as on trigglow.com, now in the plugin: open **Report a
problem** in the dock and ask about Dynamic Delay (setting it up, what the dock says, performance,
signing in). It answers right in the dialog. It's an AI and can make mistakes, so check what it
tells you before changing your setup. If it doesn't sort it out, **Create a ticket with this
conversation** sends it to the team with the context, and you can always skip Trigi and write to the
team directly. Trigi is available when you're signed in (on macOS and Linux since v1.0.0); otherwise the dialog opens on
the ticket form. The first time, a short notice ("Before you start with Trigi") explains all this,
and nothing is sent until you accept it and send your first question.

**What does Trigi see, and what doesn't it?**
It sees what you type and, only if you tick **Include OBS technical data**, a small set of numbers
and states sent once when the conversation starts: OBS and Windows versions, renderer, GPU vendor,
total RAM, encoder family, the delay's state, seconds and minimum quality, canvas and output size,
fps, OBS's lagged and skipped frames, the buffer's health and the plugin's recent problem codes.
**See what is sent** shows the exact data before you send it. The box is unticked until you tick it.
Trigi never gets your OBS log (not whole, not in parts, not summarized), and the technical data
never includes scene, source or filter names, paths, URLs, stream keys or stream details, your user
name or email, your GPU's name, hardware IDs or IPs. Your messages and the technical data are
processed by Trigglow and an AI provider listed in the
[Privacy Policy](https://www.trigglow.com/legal/privacy). Don't type passwords, stream keys, overlay
URLs or payment details. The OBS log only goes with a ticket if you tick its box, which you can do
once you've seen it in **See what is sent**. Paths with your Windows user name, URLs, keys, emails
and IPs are hidden in it; names of scenes, sources, audio devices and windows can still show up, so
check it there first.

**How long are my conversations with Trigi kept, and how do I delete them?**
Conversations with Trigi and the technical data you choose to send are kept for up to 12 months and
then deleted. The history you see in OBS is only in memory and disappears when you close the
dialog (**Clear conversation** only clears that view). To delete them earlier, on trigglow.com go to
Settings → Privacy & data → Conversations with Trigi → **Delete my Trigi conversations**, or ask
support. A conversation you turned into a ticket stays in that ticket's thread, like the rest of the
ticket.

**What network calls does the plugin make?**
On every OBS start, one check of your sign-in with api.trigglow.com (plus a renewal when it's due)
and one check of this repository's latest release on api.github.com. Everything else happens only
when you use it: signing in, signing out, asking Trigi and sending a report. All of it over HTTPS,
the sign-in only ever sent to api.trigglow.com, and the plugin only opens its own fixed pages on
www.trigglow.com and GitHub. In stream mode your stream goes from your computer straight to the
service set in OBS; no Trigglow server is in the path.

**Do I need Trigglow's Stream Deck plugin?**
No. The plugin's ten OBS hotkeys (toggle, on, off, back to live, +5 s, -5 s, dump, three presets)
work with the Stream Deck's own "System: Hotkey" action. Trigglow's Stream Deck plugin adds keys
that show the delay's state, over obs-websocket (vendor `trigglow-dynamic-delay`, which any
obs-websocket tool can use too). See `docs/STREAM_DECK.md`.

**Is it a finished product?**
On Windows, yes: **v1.0.0** is the first stable release, meant for real live use, with a
deliberately focused scope (see `docs/ROADMAP.md` for what comes next). The macOS and Linux builds
are still alpha: they build and pass CI, but haven't been run on a live stream yet, and they only
have scene mode. Stream mode is new in 1.0.0 and hasn't been run on a live stream to a real
platform yet (see "Is stream mode tested?" above). One risk
area — a GPU crash observed under an earlier design — that's believed reduced but not yet
specifically re-confirmed (`docs/SPEC.md` §7).
