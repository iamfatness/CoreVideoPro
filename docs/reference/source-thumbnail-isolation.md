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
pixel copy, second Zoom subscription, or new compositor device is involved.
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


October 15 presentation investigation (#916): the owner confirmed Sources was
open and slow during a 15-second EventPipe sample of the installed shell. The
presentation callback consumed 10.65 seconds, including 9.35 seconds in the D3D
path, while API snapshots showed fresh incoming feeds. This places the measured
bottleneck downstream of receipt; it does not identify every individual driver
wait or prove physical scanout.

Measure every source host. The original fixture watched only the first row and
Inspect, masking row-order starvation: a six-second baseline committed 98 frames
in the first row but only 5–11 in several later rows, with busy Present refusals.
A collapsed Multiview also kept presenting. Source monitors now request
per-swap-chain frame-latency credits rather than competing for the shared D3D
device's default presentation budget. They poll their own readiness handle with
a zero timeout before touching the backbuffer; the handle is closed when its
chain retires. Program/Preview retain their established queue and Present policy.
All hosts stop submission when a page ancestor is collapsed or the XAML host
is hidden. Hidden views retain their resources for resumption.

Microsoft documents the shared-device presentation budget and recommends at
most four simultaneously updated XAML swap chains:
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-multiple-swap-chains
https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.controls.swapchainpanel
The Sources workspace currently has ten thumbnails and Inspect. Giving each
chain credits removed the measured row-order starvation in isolated runs, but
serial Present CPU stalls remain a qualification concern. Experiments increasing
the global queue, changing sync interval, adding buffers or changing alpha mode
are not product changes. None established complete recovery.

The full-workspace probe records every host's commits and maximum commit gap,
checks that hidden views submit nothing, and retains failing speed/motion checks.
QA counters/readback are disabled in normal operation. GPU load varied up to 93%
while another application was active during the later comparisons; those runs
cannot isolate candidate frame-rate improvement. Do not downgrade source quality,
relax acceptance thresholds, close #916, or claim the installed app is repaired
from these results. Physical display cadence, sustained output acceptance and
complete source-motion qualification remain open. The installed app/session and
settings were preserved; no desktop input or elevation was used for this trace.
