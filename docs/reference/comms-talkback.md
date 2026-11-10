# Comms: Zoom talkback (the ZComms station in Pro)

_Added 2026-11-09 (branch `codex/comms-talkback`). Design shared with CoreVideo
Edge: Edge `docs/superpowers/specs/2026-11-09-comms-design.md`._

The **Comms** page lets the operator cue talent over Zoom talkback from Pro
itself: one grid of person cells where a cell IS a talk key, ALL CALL above it,
LATCH / EDIT TALENT / SETTINGS / LEAVE below, lamps LINK / MTG MIC / CHANNEL /
TX and a one-line ops strip. It is the ZComms talkback station in Pro's look.
Intercom-in-Zoom is a product pillar (marketing positioning); keep it native.

## Where it runs (forced by Zoom)

Talkback is sent by a participant that is **host or co-host**, and the SDK is
one participant per process. So the station is Pro's own Zoom engine
(`native/zoom-engine`, `EngineComms` + `engine/comms/**`), the same participant
that receives video. Consequences:

- Pro's Zoom account must be host or co-host; the page says so otherwise and
  video keeps working.
- The engine builds against **Meeting SDK 7.1.5** (talkback controller).
- Guests need the Zoom desktop/mobile client (web cannot receive talkback;
  they are listed as NO TALKBACK with the fix) and must be in the station's
  room (no cross-breakout delivery; this slice is not breakout-aware).
- **Pro joins are never capture-only**: the operator may speak in the meeting,
  so Comms never installs the silent virtual mic and never unmutes. Talkback is
  delivered only while the meeting mic is OPEN (ZComms law 1); MTG MIC shows
  it and the reason says "Talkback is silent: …" while it is muted. Edge
  (capture-only) differs; see `native/zoom-engine/README.md` "Comms".
- Comms failures never touch subscriptions, ISO, raw media or outputs.

## Data path

```
CommsPage (WinUI) ── CommsViewModel ── IZoomCommsTransport (MediaCoreBridgeService)
   │ press/release, digits, Space          │ SendCommsAsync(verb, payload)
   ▼                                       ▼
MediaCoreSupervisor.SendCommsAsync ──stdin──▶ core JsonRpcServer (comms-*: LOCK-FREE,
                                                 no coreMutex, no assembly slot)
                                               → MediaCore::zoomCommsCommandUnlocked
                                               → ZoomEngineRuntime::commsCommand
                                                 (ZoomCommsRelay validates; sender queue)
                                               ──pipe──▶ engine comms_* line
engine comms_state ──pipe──▶ ZoomEngineRuntime (re-keyed camelCase, held latest)
   → drainFrameEvents "zoom-comms-state" (zoom pump, no coreMutex; repeated 1/s
     while held because that lane may drop) ──stdout──▶ supervisor
   → ZoomCommsStateReceived → StudioViewModel (UI thread) → CommsViewModel.ApplyComms
```

`comms-key`, `comms-latch` and `comms-release-all` are **urgent** in the core
mailbox (`CommandMailbox::urgent`) and on the EngineHost wire, so a key-up is
never refused for capacity.

## Wire contract (shell ↔ core)

Requests are ordinary core requests: `{"id","type":<verb>,"payload":{...}}`.
The verbs and their fields are **CoreVideo Edge's** (`docs/PROTOCOL.md`
"Comms" in Edge), relayed by the same code (`src/modules/ZoomCommsRelay` is
Edge's `CommsRelay` verbatim):

| type | payload |
|---|---|
| `comms-key` | `{"slot":0..15}` or `{"all":true}`, `"on":bool`, optional `"leaseMs":0..60000` |
| `comms-latch` | same as key, no lease |
| `comms-assign` | `{"slot","participantId","on"}` |
| `comms-mic` / `comms-sidetone-out` | `{"device":"<name>"}` (`""` = system default) |
| `comms-sidetone` / `comms-tone` | `{"on":bool}` |
| `comms-gain` | `{"db":-20..20}` |
| `comms-release-all` | `{}`: every held key and ALL CALL up; latches stay |

Answer: `{"ok":true,"type":<verb>}` or failure code `comms-refused` with the
operator text in `error.message` (shown verbatim in the ops strip). Releasing
(`on:false`, release-all) is never refused, even with no engine.

State: unsolicited `{"type":"zoom-comms-state","comms":{...}}`; `comms` is the
engine's last `comms_state` re-keyed to camelCase (Edge's state object:
`available`, `reason`, `lamps{link,meetingMic,channel,tx}`, `room`,
`channels[16]{slot,label,listeners,ready,keyed,latched,room,reachOk,reachDark}`,
`roster[]{participantId,name,talkback,room,channels}`, `mic{device,devices,
peakDbfs,gainDb,sidetone,sidetoneDevice,sidetoneDevices,tone}`,
`stats{framesSent,channelSends,underruns,sendErrors}`, `log[]`).
`"comms":null` = the report was dropped (Leave, engine gone, core restart);
the shell then shows no key as live. Parsed by `ZoomCommsProtocol`.

**Deviations from Edge's node protocol**: arguments ride in `payload` (Pro's
request convention) instead of top-level fields; the state arrives as its own
`zoom-comms-state` fact instead of a `comms` member of a state line. Names,
fields and values are identical.

## Leases and release

A held key is **leased** (Edge contract): every press sends `leaseMs:1500`
and the shell re-sends it every 500 ms while held (`StudioViewModel.Comms.cs`
DispatcherQueueTimer → `CommsViewModel.RenewLeasesAsync`). A hung shell
therefore drops its keys in 1.5 s. Every held key goes up when: focus leaves
the page, the window deactivates (`MainWindow.OnWindowActivated`), the
operator navigates to another tab, the meeting ends, or the report is dropped.
Shell shutdown sends `comms-release-all` (`PrepareForShutdown` → `ShutdownComms`).
If the shell dies, the core's stdin closes, the core exits and the engine
process goes with it (talkback ends with the participant).

## UI rules (do not regress)

- `TalkKey` (Controls): press = down, release/capture-loss/cancel = up, with
  pointer capture; **Click and Command are never used**; hover/pressed visual
  states are suppressed so the colour is always the engine's truth; Space and
  Enter bubble to the page.
- Never rebuild a control under the operator's finger: cells, people, device
  pickers and the ops history are `ObservableCollectionSync`-synced (no
  `Reset`), and only on a structure-signature change; values update in place.
  Tests pin it (`CommsViewModelTests`).
- A cell is never "on air" unless the engine reports it keyed **and** someone
  confirmed in the channel. Keyed with nobody = armed.
- Digits 1-9 (first nine reachable cells) and Space (ALL CALL) are
  **panel-scoped** (`CommsPage` PreviewKeyDown: focus inside the page, not in a
  text field, no modifier, auto-repeat ignored). Never global.
- Colours are Pro tokens only (`CommsBrushConverter`): **on air = filled
  `StudioAirBrush`** (Pro's "air red - on-air, record") with
  `StudioBackgroundBrush` text; armed = `StudioWarningBrush` outline and text;
  ready = `StudioTextBrush`; idle = `StudioTextSecondaryBrush`; unreachable =
  `StudioDimBrush`; lamps on = `StudioLiveBrush`.
- Explicit states: not in a meeting; waiting for the engine's report; not up
  (verbatim reason, host/co-host hint); desk.

## Testing

- Native: `CommsRelay*`, `CommsCore*`, `CommsProtocol*`, `MicGuard*`,
  `ZCommsPorted*`, `ZoomEngineRuntime.Comms*` in `corevideo-native-tests`
  (the shim takes ONE `--gtest_filter` pattern).
- Shell: `CommsViewModelTests` (WinUI.Tests): the JSON each action sends,
  leases, release-all, structure gates, states, colour tokens.
- End to end without Zoom: `scripts/probe-comms.mjs` drives the real core
  binary over stdio against `corevideo-zoom-engine-fake` (join, wait for the
  bank, key/latch/assign/tone, read the `zoom-comms-state` facts).
- **Not proven**: real talkback against Zoom (needs a live meeting with this
  account as co-host and desktop-client guests), audio at the guest, the 7.1.5
  raw video/ISO path in a live take.
