# Graphics logo upload — #1012

The owner found this after the #1007 installed Graphics checks. Those checks
qualified standalone full-canvas PNGs, not replacement of the embedded image in
a declarative logo template. The compatibility inspector had GraphicsImageControl;
the declarative Content inspector exposed only packaged image IDs. Native
readRevision also rejected image references on declarative instances.

The Content inspector now exposes Upload logo PNG / JPEG for templates with the
existing declared logo asset. The compatibility uploader uses the same guarded
ImportLogoAsync service. Verified upload installs the immutable image package and
edits only the unchanged private draft; it enables the existing logo visibility
field. Navigation or edits during import remain authoritative. Cue/Show/Update
are explicit. No live media or default template manifest is rewritten.

Native revisions retain the verified image reference and content hash, and carry
it into the cached declarative raster input. Only the already declared logo slot
uses the replacement; other assets and template declarations retain validation.
Private authoring resolution carries the same reference. Existing resource budgets,
immutable accepted copies, missing-image rejection and premultiplied WIC alpha
remain in force. No image file is reread on a rendering caller.

Regression coverage includes import/edit races, failed upload, undo, compiled
logo-uploader reachability, verified native Preview/Program reference retention,
actual D3D transparent/half-alpha logo pixels and contain aspect ratio, and
accepted Program retention across private replacement. The first pixel trial
sampled the curated entrance fade; its failure is retained. The alpha test now
explicitly selects no entrance/out transition to isolate alpha from animation;
production transitions are unchanged. Installed acceptance is pending.


Installed trial 06fdb17 exited setup at the first workspace Show: native Cue
preparation was ready, while the workspace's last UI observation still had no
accepted Cue. This was a refusal, not a passed logo check. Command execution now
observes the current bridge snapshot before evaluating readiness, retaining the
same guards and revision checks. A deterministic test supplies native readiness
before a UI observation and checks the exact Cue revision consumed by Show.
Original installer/launch/hash receipts and the failed API run are retained in
`artifacts/graphics-logo-1012/trial-06fdb17`; repeat installed acceptance is pending.
