# Sources thumbnail identity and GPU publication (#882)

Sources input thumbnails bind a dedicated, native per-source shared texture.
They do not crop the continuously changing Multiview atlas or capture the desktop.
An atlas handle can stay constant while its source cells move; independently
applying a new crop cannot guarantee that the current texture has that layout.

`ShowInputsCoordinator` forwards participant-shared-texture structural changes to
`MultiviewInputRowsProjection`. Native participant IDs normalize to `zoom:<id>`,
while capture and media IDs already carry their scheme. The cache admits only
currently assigned sources and retires descriptors on unassign, unavailable
input, source incarnation change, or engine stop. Its size is bounded by the ten
stable operator rows. Multiple slots for one source share its descriptor.

`SourcesInputsPage` binds `Inspector.PreviewSurface` with full-source aspect fit.
`VideoSurfaceHost` uses the GPU path for the `source-thumbnail:` namespace,
keeps thumbnail presentation capped at 30 Hz, hides an old identity's backbuffer
before rebinding, and releases the old ingest when its handle changes. No CPU
pixel copy or second Zoom subscription is involved; the native compositor is unchanged.
Multiview metadata supplies tally only; wall removal/reordering cannot change
which texture belongs to a thumbnail.

On the isolated monitor path, source export demand includes at most ten assigned
Show Input sources even when a composite Preview is active. Existing Preview
fallback demands remain supported. `SourcesInputsPage` owns ephemeral interest
only while loaded and `IsInputsTab` is true. Tab exit/unload sends release and
clears row surfaces. `set-source-preview-demand` leases this optional Inspector
work for three seconds, renewed once per second while the page is visible and
the core is healthy. A crashed shell or lost release cannot leave the work alive.
Renewing changes only the lease deadline, never source admission/subscriptions,
render-plan generation, source preparation, or persisted production settings.
Engine stop/recovery drops local preview state and restarts interest only if the
consumer remains visible. Inactive Inspector interest does not stop source media
or remove independent Grade/PreviewFallback demands.
The existing source grade map travels in the render plan and is baked by
`effectiveParticipantGrade`; source exports remain at their native dimensions.

A source epoch must describe the pixels actually published, not the newest frame
submitted to an asynchronous exporter. `D3DDecoupledExport` carries epoch through
its input slots and prepared image. On epoch change its worker replaces only the
consumer texture, preserving its D3D device and internal slots. It publishes a
coherent handle/frame/epoch descriptor after handing over the output key. An old
consumer holding its mutex cannot delay the new incarnation, and a new handle
cannot reuse the shell's cached image for the old incarnation. Legacy DXGI shared
handles remain resource-owned; do not call `CloseHandle` on them.

The optional `sourceEpoch` protocol field fences delayed descriptors within a
producer lifetime. A core stop or new Zoom roster epoch resets this floor; numeric
epochs from different producer lifetimes must never be ordered. Missing epoch
fields remain compatible as epoch zero. A changed roster incarnation clears the
preview until a newer native epoch qualifies it (when epochs are supported).

Zoom roster `sourceGeneration` and native image `sourceEpoch` are independent
identities. The former identifies the roster incarnation; the latter also changes
on native texture/resolution replacement. A participant texture additionally carries
its optional positive `sourceGeneration`, captured with the decoded pixels. Never
stamp an asynchronous publication with the latest roster generation. The exporter
first qualifies the published native epoch against the frame carrying that generation.

A fresh texture can arrive before its matching roster fact. The projection stages
one future-generation descriptor without displaying it; when the matching roster
arrives, it promotes that descriptor immediately. Clearing it and demanding an
additional native epoch would indefinitely reject valid subsequent announcements
until another resize/rejoin. Older-generation descriptors stay rejected even if
their native epoch is higher. Legacy generation-zero producers keep the existing
native-epoch fence. Engine/roster lifetime changes reset both identity domains.

Regression coverage exercises stable row identity across removal/reassignment,
wall changes independent of pixel identity, capture disconnect/reconnect, old
incarnation rejection, producer reset, and legacy protocol parsing. The native
GPU exporter regression holds an old consumer key and reads the new incarnation's
exact pixel from its new handle while checking device reuse. Packaged visual
acceptance must exercise distinct moving feeds in the actual Sources page;
projection tests alone cannot establish correct displayed pixels.


The opt-in `--verify-inspect-video <report.json>` probe uses an offscreen Sources
workspace, ten owned 1080p keyed-mutex GPU sources and actual swap-chain readback.
It checks source switching, idle-UI presentation and changing pixels under a stable
handle. `--verify-inspect-native <core.exe> <report.json>` exercises native BGRA
exports and the sibling synthetic Zoom engine's I420 feeds, then the complete
StudioWorkspace with live model callbacks. It refuses to run without the synthetic
engine. These processes use isolated logs/in-memory production settings and do not
connect to the installed core, camera or meeting. Readback is explicit QA only,
never part of the production presentation loop. Backbuffer pixels and finite motion
checks do not establish physical display cadence or sustained output acceptance.


## Presentation ownership and pacing (#916)

An owner-confirmed slow Sources view was traced for 15 seconds in the installed
shell. `TryPresentPendingSharedHandle` consumed 10.65 seconds, including 9.35
seconds in the D3D path, while incoming source frames kept advancing. The
all-row fixture then exposed two independent presentation defects: later rows
starved on shared-device credits, and collapsed pages kept submitting work.
After those were corrected, serial driver Present waits still limited the
complete page to about five updates/second. Queue depth alone did not fix this.

`VideoSurfaceRenderPresenter` now publishes immutable geometry/handle requests
to four persistent render threads. XAML never creates devices, acquires video
mutexes, copies pixels, waits for DXGI, reads pixels or joins a render thread.
Each thread owns a stable device group and all its D3D/DXGI calls, including
copy, presentation, resize, probe readback, loss recovery and resource retirement.
`Direct3D11InteropService` retains a default group for legacy/test callers.
There is no per-row device creation/destruction on view churn.

`SharedTextureRenderAssignments` assigns by **shared handle**, not label.
Thumbnail, Inspect and a Preview alias must share one consumer and its private
latest-frame texture. Least-loaded assignment spreads distinct handles across
persistent groups. Program/Preview retain sync interval 1 and their established
queue flags; source monitors use independent one-frame credits and sync interval
0 so DWM receives the latest image without serial source-vsync waits. Source
requests are capped at 30 Hz using accumulated high-resolution deadlines;
rounding each interval to milliseconds can accidentally reduce the rate on
some display cadences. Native source dimensions and GPU quality are unchanged.

Each host has a single pending mailbox. A newer request replaces the pending
one instead of building a stale-frame queue. Close rejects further offers and
native resources retire on their owning render thread. Swap-chain creation
transfers an AddRef'd lease to a nonblocking UI callback. The callback checks
host disposal and chain revision before attachment, then releases its lease.
UI path visibility additionally checks the exact requested handle and attached
revision: an old completion cannot reveal a retired source. Fractional panel
geometry and tile-crop transforms travel with the request. Hidden ancestors or
a hidden XAML host stop submissions; restoring the page resumes from live pixels.
Device-loss reporting aggregates all groups; recovery reset is posted to each
owning thread rather than mutating its state from XAML.

Microsoft's threading contract is why moving only `Present` to `Task.Run` while
leaving immediate-context work on XAML is unsafe:
https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro
https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices
The UI never waits for a render-thread DXGI call, avoiding the message-pump
wait cycle described there. Microsoft also documents the shared-device queue
budget and simultaneous XAML swap-chain performance limits:
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-multiple-swap-chains
https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.controls.swapchainpanel

## Finite validation and limits

The all-row probe requires at least 150 committed images per visible source
monitor over six seconds, maximum gaps below 250 ms, no hidden-view commits,
and actual changing pixels in thumbnail and Inspect. It also checks hide/restore
and adds a deliberate 30 ms delay to every Present for 2.2 seconds: UI dispatcher
callbacks must continue with a maximum gap below 150 ms. A separate GPU probe
checks source selection, idle-UI motion, exact red/green/blue identity and a
Preview alias of the same GPU handle. Readback/timing/fault injection are QA-only.

The narrowed predecessor failed with 29 commits/view in six seconds and a
374 ms worst gap. The first strengthened worker run committed 169–170 images
in every visible source monitor over six seconds; the dispatcher maximum gap
was 80.4 ms normally and 65.0 ms with the injected driver delay. Hide/restore
passed. These are local RTX 4090 backbuffer and dispatcher results under variable
competing GPU load, not physical-scanout measurements or a fleet guarantee.
A read-only NVAPI query also found a user global pre-render limit of 1; no driver
settings were changed, and its independent causal effect was not A/B tested.

No installed app, real meeting, camera registration or production setting is
changed by these owned offscreen probes. Packaged visual review and sustained
native-output zero-miss acceptance remain separate; do not infer them from
source-monitor commit counts or close the issue before its merged acceptance.
