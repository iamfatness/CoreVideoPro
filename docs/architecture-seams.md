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

> **PR1** (this change) extracted **MagicScene** automation (`MagicSceneCoordinator` +
> `IMagicSceneHost`) and the **Transport status formatters** (`TransportStatusFormatter`).
> **PR2** = Transport orchestration (record/stream/engine/Take async bodies) behind an
> `IMediaCoreBridge` DI seam — gated because `StudioViewModel` is not constructible in tests
> today. **PR3** = ShowInputs. **PR4+** = the C++ hot core (MediaCore / compositor /
> JsonRpcServer).

**StudioViewModel LOC:** `StudioViewModel.cs` **15,028 → 14,176** (the monolithic file), with
~950 lines of behavior relocated into independently-testable types
(`TransportStatusFormatter.cs` 586, `MagicSceneCoordinator.cs` 363, `IMagicSceneHost.cs` 80)
plus a 203-line façade partial (`StudioViewModel.MagicScene.cs`) that keeps XAML x:Bind paths
unchanged. `[ObservableProperty]` 147 → 134, `[RelayCommand]` 72 → 69.
