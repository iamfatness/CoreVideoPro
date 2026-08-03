# Architecture seams

_Companion to `README.md` (as-built) and `CLAUDE.md` (how not to break it). This is the
short map of the process boundaries and where new spine capability plugs in — read it before
adding ISO / NDI / SRT / browser work so pixels/PCM land in the core, not the ViewModel._

## The four processes (who owns what)

CoreVideo Pro is **not a web app** — it is four cooperating OS processes:

| Process | Binary | Owns | Never owns |
|---|---|---|---|
| **WinUI 3 shell** | `CoreVideoPro.WinUI.exe` | Operator console: XAML UI, ViewModels, command intent, rendering shared textures | Real-time pixels/PCM |
| **C++ media core** | `corevideo-native.exe` | **All real-time pixels + PCM**: D3D11 compositor, audio mixer, recorder, output senders, vcam publisher | The Zoom SDK; UI |
| **Zoom engine** | `corevideo-zoom-engine.exe` | Zoom Meeting SDK; writes raw **I420** frames + PCM to shared memory | Compositing; output |
| **Virtual camera DLL** | `corevideo-virtualcam.dll` | Serving the program NV12 frame to the Windows Frame Server (system-wide webcam) | Producing frames (the core does) |

The load-bearing rule (see `CLAUDE.md` north star): **the shell owns no real-time media.**
It sends commands and renders textures the core hands it. Any per-pixel or per-sample work
belongs behind the core boundary, on the GPU or a dedicated audio worker — never in a
ViewModel.

## Command / snapshot flow

```
 operator action ─► StudioViewModel (+ sub-VMs) ─► JSON-line command ─► corevideo-native
                                                                            │
   XAML x:Bind ◄─ ViewModel state ◄─ snapshot-apply ◄─ JSON-line snapshot ◄─┘
                                                        (periodic + on change)

 pixels:  zoom-engine ─I420 SHM─► core ─keyed-mutex DXGI shared texture─► shell surfaces
          core ─NV12 file-backed SHM─► vcam DLL ─► Frame Server ─► Zoom/Teams/OBS
 audio:   zoom-engine ─PCM SHM─► core mixer ─► recorder / RTMP / monitor / NDI / SRT
```

- **Commands** are JSON lines over named pipes (control intent: join, set-scene, start-record,
  configure-output, browser-add, …).
- **Snapshots** are JSON lines the core publishes; the shell's snapshot-apply path maps them
  onto ViewModel state (structural-only diffs — never rebuild a bound collection at frame
  rate; that is the `0xc000027b` fail-fast class).
- **Video** crosses processes as keyed-mutex DXGI shared textures (program/preview/multiview)
  and shared-memory I420 (Zoom) / NV12 (vcam).

## Where spine features plug in

Spine capability (ISO record V+A, NDI, SRT, browser sources) is **core work behind typed
commands**, not ViewModel pixel work:

- **New output/ingest** = a core **adapter** (e.g. `BrowserSourceHostAdapter`, output senders)
  owned by `MediaCore`, driven by a typed command in `native-core/src/protocol.ts` +
  `JsonRpcServer`, with health surfaced in the snapshot. The shell adds a Sources/Transport
  toggle and reads the health node — it does not touch frames.
- **Isolation pattern for untrusted/heavy producers** (Zoom engine, browser host): a separate
  process that publishes into seqlock SHM, supervised with backoff + give-up (see
  `CaptureReaderStallPolicy` / `PluginHostRespawnPolicy`).
- **The ViewModel's job** for a spine feature is intent + status: send the command, bind the
  health readout. If a change wants to move a bound-collection rebuild off the UI thread or
  change its rate, stop — that is the crash class, not a feature.

## StudioViewModel strangler status

`StudioViewModel` is the shell's god object. It is being reduced by **vertical-slice
extraction** (strangler pattern; new behavior goes in focused `MagicScene*` / `Transport*`
types, never new methods on the god file — `FOCUS_PLAN.md` §9):

> **PR1** extracted **MagicScene** automation (`MagicSceneCoordinator` + `IMagicSceneHost`) and
> the **Transport status formatters** (`TransportStatusFormatter`).
> **PR2** (this change) introduced the **`IMediaCoreBridge`** DI seam and extracted **Transport
> orchestration** — the Engine on/off, Take, Record, and Stream async command bodies + their
> in-flight guards, #286-class rollback, backpressure-retry, and sender-proof helpers — into
> `TransportCoordinator` behind `ITransportHost` + `ITransportDispatcher`. The coordinator is
> independently constructible (fake `IMediaCoreBridge` + fake host), so transport orchestration
> now carries its first-ever characterization tests (`TransportCoordinatorTests`). Move-only:
> StudioViewModel keeps the generated `[RelayCommand]` objects as thin forwarders so XAML x:Bind
> and the ~15 external `NotifyCanExecuteChanged` pokes are unchanged; the bound transport state
> (Recording/Streaming/OutputStatus/…) stays `[ObservableProperty]` on the god file and the
> coordinator writes it through `ITransportHost`. Deferred to a follow-up (kept on the façade to
> bound the host surface + honor no-XAML-churn): the readout-projection `RefreshTransportState`/
> `ApplyConfiguredOutputReadouts`, the bound `TakeTransitionMode`, and the vcam enable/mirror/name
> handlers. **PR3** = ShowInputs. **PR4+** = the C++ hot core (MediaCore / compositor /
> JsonRpcServer).

**StudioViewModel LOC:** `StudioViewModel.cs` (the monolithic file) **14,423 → 13,817** after
PR2; the four transport command bodies + their five async helpers + eight now-dead static
wrappers + the two in-flight fields relocated into `TransportCoordinator.cs` (651, incl. docs) /
`ITransportHost.cs` (126) / a 165-line façade partial (`StudioViewModel.Transport.cs`) that keeps
XAML x:Bind unchanged, plus the assembly-level `IMediaCoreBridge.cs` (95) seam.
`[ObservableProperty]` 136 and `[RelayCommand]` 72 are UNCHANGED (forwarders preserve every bound
member). (PR1 took the file 15,028 → 14,176 for MagicScene; the 14,423 baseline here includes the
intervening ISO-1..4 work, #316/#318/#320.)
