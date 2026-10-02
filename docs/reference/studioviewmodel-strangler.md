# StudioViewModel strangler (maintainability, FOCUS_PLAN §9)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`StudioViewModel` (the shell god object) is reduced by **vertical-slice extraction** — new
behavior goes in focused `MagicScene*` / `Transport*` types, never new methods on the god
file. **PR1 (done):** `MagicSceneCoordinator` + `IMagicSceneHost` (Magic Scene / Set & Forget
automation) and `TransportStatusFormatter` (pure transport status/rollback/validation
statics). Move-only, XAML x:Bind unchanged (same-named forwarders on StudioViewModel + a
PropertyChanged bridge; StudioViewModel implements `IMagicSceneHost` over `this`, the
Transport/Overlays sub-VM pattern). The extracted types are independently constructible so
they carry real characterization tests (`MagicSceneCoordinatorTests`) — StudioViewModel itself
is still NOT constructible in tests (field-init `DispatcherQueue.GetForCurrentThread()` + ctor
hard-`new()`s ~10 services + launches the core; a later DI-seam PR). **PR2 (done):** the
`IMediaCoreBridge` DI seam + `TransportCoordinator` (`ITransportHost` + `ITransportDispatcher`)
owning the Engine/Take/Record/Stream async command bodies, in-flight guards, #286 rollback
(scenes + the media selection the Take moved, T1.3),
backpressure-retry, and sender-proof — constructible + characterization-tested
(`TransportCoordinatorTests`). Same move-only façade rules: the `[RelayCommand]` objects stay
generated on StudioViewModel as thin forwarders (XAML + external `NotifyCanExecuteChanged` pokes
unchanged); bound transport state stays `[ObservableProperty]` on the god file, written through
`ITransportHost`. **PR3 (done, stacked on PR2):** the **ShowInputs** cluster —
`ShowInputsCoordinator` behind `IShowInputsHost` (+ injected `IShowInputRosterStore`/`IMediaCoreBridge`)
owning roster persistence, the signature-gated roster→`ShowInputEditors` projection, auto-assign,
unassign/take-offline, SRT-ingest add/remove, and the per-source **ISO selection** (the ISO-4
ISO×ShowInputs integration). The coordinator OWNS `ShowInputEditors` (StudioViewModel exposes it via a
same-named forwarder property so x:Bind is unchanged) + `IsoSelectedSourceIds` (v8 persistence routes
through it); constructible + characterization-tested (`ShowInputsCoordinatorTests`, incl. an
ISO-survives-a-roster-refresh test). The 0xc000027b signature-gating + in-place diff-update + ISO
re-projection are preserved exactly. **Deferred (verification finding):** dual-capture selection is
entangled with capture-fleet enumeration + `[ObservableProperty]`-bound → a future **CaptureFleet**
extraction, not the roster cluster. **PR4+** = the C++ hot core.
