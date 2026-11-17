*[Versión en español](STREAM_DECK-es.md)*

# Trigglow Dynamic Delay + Stream Deck

There are two ways to drive the delay from a Stream Deck (or any other controller):

- **OBS hotkeys**, with the Stream Deck's own "Hotkey" action. Works with any Stream Deck software
  and nothing else installed, but the key can't show the delay's state.
- **obs-websocket**, the remote control built into OBS. The plugin registers the vendor
  `trigglow-dynamic-delay` (v1.0.0). Trigglow's Stream Deck plugin uses it for its Dynamic Delay
  keys, which show the delay's state on the key; any tool that speaks obs-websocket 5 can use it
  too.

Both work in stream mode and in scene mode: the plugin sends each command to stream mode when it
handles the current (or next) stream, to scene mode otherwise.

## With hotkeys

### Step 1: set the hotkey in OBS

1. `Settings → Hotkeys`.
2. Type "Trigglow" in the filter at the top.
3. You'll see these entries:
   - **Trigglow: Toggle Dynamic Delay** (the one most people want)
   - **Trigglow: Enable Dynamic Delay**, **Trigglow: Disable Dynamic Delay**
   - **Trigglow: Back to live (delay off)**
   - **Trigglow: Delay +5 s**, **Trigglow: Delay -5 s**
   - **Trigglow: Dump what viewers haven't seen** (stream mode only)
   - **Trigglow: Delay preset 1**, **2** and **3**
4. Click the field next to the one you want and press the key combination (e.g. `Ctrl+Alt+D`).
   Avoid combinations you already use for something else.

The first three existed before 1.0.0 and keep their internal names, so bindings you set before
carry over. In an OBS in another language the names are translated (in Spanish, "Trigglow:
Activar/desactivar Dynamic Delay"...).

### Step 2: map the button on the Stream Deck

1. Open the Stream Deck software (Elgato).
2. In the actions panel, find the **System** category and drag the **Hotkey** action onto a button.
3. Set it to the same key combination as in step 1.
4. Optional: change the button's icon and title to something you'll recognize, like "Delay".

### Step 3: try it

1. With OBS open (you don't need to be live for the first try), press the button.
2. The "Trigglow Dynamic Delay" dock changes state:
   - **Stream mode**: "Delay armed: your next stream starts 30 s late" before you stream; while
     streaming, "Delay on: viewers see the stream 30 s late" or "Live, no delay".
   - **Scene mode**: "Preparing delay" with a countdown while the buffer fills, then "Delay on".
3. Do it live without worry: in neither mode does the stream reconnect or cut.

### Notes

- If your Stream Deck sends the keystroke to the system rather than to a window, OBS has to be able
  to receive global shortcuts. OBS registers its hotkeys system-wide on Windows and macOS, so it
  usually works with OBS in the background, but your OS's accessibility or permission settings can
  change that.
- A combination you also use elsewhere can trigger both. Pick an unusual one.

## With obs-websocket

obs-websocket ships with OBS (Tools → WebSocket Server Settings: turn it on and note the port and
password). The plugin registers its vendor when OBS loads it; the OBS log says
`obs-websocket vendor "trigglow-dynamic-delay" registered`, or
`obs-websocket not available` when obs-websocket isn't there.

Send `CallVendorRequest` with `vendorName: "trigglow-dynamic-delay"`, a `requestType` and, when
needed, `requestData`:

| requestType | requestData | Does |
|---|---|---|
| `GetStatus` | — | only returns the status |
| `EnableDelay` | — | delay on (needs the free account, like the dock) |
| `DisableDelay` | — | delay off |
| `ToggleDelay` | — | on/off |
| `SkipToLive` | — | back to live: delay off |
| `SetDelay` | `{"seconds": 45}` | sets the seconds: 1-600, live in both modes. 0 or less is ignored |
| `AdjustDelay` | `{"delta": -5}` | adds the seconds (negative lowers them), same ranges |
| `DumpDelay` | — | stream mode: the seconds viewers haven't seen are never sent; `dumped` is false when there was nothing to drop (delay off, not streaming, or scene mode) |
| `ApplyPreset` | `{"index": 0}` | preset 1 (`0`), 2 (`1`) or 3 (`2`): its seconds, delay on. Same in scene mode |
| `SetDelayedScene` | `{"sceneName": "Chat"}` or `{"slot": 2}` | all of OBS (v1.0.0), delay on: puts that delayed scene in the delay; viewers see the switch when the delay catches up. `selected` is false when the delay is off or the scene isn't one of the delayed scenes |

Every answer's `responseData` has `ok: true` and the status:

| Field | Meaning |
|---|---|
| `mode` | `stream` or `scene` |
| `state` | stream mode: `idle`, `live`, `delayed`, `draining`; scene mode: `inactive`, `filling`, `active`, `error` |
| `delayOn` | the delay is on (or armed for the next stream) |
| `delaySeconds` | the seconds set |
| `appliedSeconds` | what viewers get now (0 when live) |
| `behindSeconds` | stream mode: how old what the relay sends is, to 0.1 s |
| `ramMegabytes` | stream mode: the RAM kept, to 0.1 MB |
| `delayedScenes` | all of OBS's delayed scenes, `[{"sceneName": "..."}]`, in the dock's order |
| `delayedScene` | the one in the delay now (empty while the delay is off) |
| `dumped` | only in `DumpDelay`'s answer |
| `selected` | only in `SetDelayedScene`'s answer |

The plugin also emits the vendor event **`StatusChanged`** (subscribe to vendor events) with
`mode`, `state`, `delayOn`, `delaySeconds` and `appliedSeconds`, only when one of them changes.
While stream mode is catching up after a raise, `behindSeconds` moves without an event: poll
`GetStatus` (once a second is enough) if you want to show it.

Example request (obs-websocket 5, op 6):

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "dd-1",
    "requestData": {
      "vendorName": "trigglow-dynamic-delay",
      "requestType": "SetDelay",
      "requestData": { "seconds": 45 }
    }
  }
}
```

Requests run on OBS's UI thread, like the dock's buttons, and the answer comes after the command
has been applied.

## Trigglow's Stream Deck plugin

Trigglow's Stream Deck plugin is getting Dynamic Delay keys built on this vendor: toggle, back to
live, a preset (optionally turning the delay on) and +/- a step, each showing the delay's state.
It connects to obs-websocket with the host, port and password you enter in its settings. Until
that version is out, use the hotkeys above.

Not tested yet: those keys against a real OBS with Dynamic Delay 1.0.0 and real Stream Deck
hardware (they were checked against a simulated obs-websocket).
