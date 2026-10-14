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

Regression coverage exercises stable row identity across removal/reassignment,
wall changes independent of pixel identity, capture disconnect/reconnect, old
incarnation rejection, producer reset, and legacy protocol parsing. The native
GPU exporter regression holds an old consumer key and reads the new incarnation's
exact pixel from its new handle while checking device reuse. Packaged visual
acceptance must exercise distinct moving feeds in the actual Sources page;
projection tests alone cannot establish correct displayed pixels.
