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


The connected advanced image selector also called Set(asset, ...) while the
managed editable-property list omitted asset. That slot is now editable through
the existing immutable scratch path, including brand-token declaration and layer
locks. Native template validation and explicit immutable-version save still gate
Cue/Update; no asset path or live template is rewritten by selecting a source.


## Final installed acceptance

Installed engineering candidate `beta-2026-10-21-g1012906d769`, code commit
`906d76972824887a7ff4d8b4854a0b97961137e0`, via the established NSIS installer
(exit 0). All 864 manifest-listed files match. The ordinary desktop shortcut
opened the exact shell/native installation in normal non-elevated session 1;
registered camera Inspect matches the desktop SID and installed candidate.
No QA frame bands or renderer/DLL overrides were used.

The original workspace Cue/Show sequence passes without native-Show bypass or
arbitrary readiness sleep. Uploaded alpha PNG -> hidden corner-logo cue -> Cue ->
Show reaches actual composition. Camera-output samples are red [253,0,0],
half-green-over-blue [0,126,126], and fully transparent/contain-letterbox blue
[0,0,253], within the existing tolerance of 8. Uploading a replacement leaves the
full accepted Program frame byte-for-byte unchanged; explicit Update reveals the
orange replacement [253,127,0]. Saved cue data contains both exact package pins.

After actual shell/native-core restart, restore imports the pinned image and
creates a hidden cue. Explicit Cue/Show again produces the orange logo. Separate
Logo accent lower-third upload passes alpha/aspect pixels, and malformed PNG
replacement is refused without losing accepted Program. Use in Studio -> LT in
retains that uploaded logo while binding the real webcam's configured name/title.
Decoded ordinary Windows-camera images for these paths were inspected locally.
No user camera images or desktop captures were uploaded to GitHub.

Seven final ordinary OS-camera consumers delivered 180 frames each (1,260 total),
including the final restored-profile Program output. This proves functional pixels,
not frame cadence. Earlier preliminary direct-native Show isolated the renderer
on 06fdb17; it was explicitly excluded from final workspace acceptance. Original
failed API/fixture trials and all prepared packages remain retained.

Final gates: 1,831 production native tests, 2,601 MediaCore / 1,722 WinUI / 102
Control tests, 55 compiled Studio/Graphics/settings checks and bundled runtime
probe. The packaged native SHA256 matches the full native suite binary:
cf0a408de22c6069c5ea3dee6ca661e4ef23df29685d07d2c9b45227bc14342a.
Later corrections affect managed command/editor/probe paths only. Compiled uploader
reachability and actual installed import service are covered; this is not a manual
Windows file-picker click test. Hosted CI is reported separately.

Evidence: `artifacts/graphics-logo-1012/installed-logo-acceptance.json`,
`*-pixel-acceptance.json`, `saved-pins.json`, `studio-camera-logo-shown.json`,
`installed-payload-verification.json`, normal-shortcut/registration receipts,
`profile-restore.json` and `final-camera-progress.json`. Owned graphics were
cleared, five original profile files restored with matching hashes, and the normal
shortcut reopened. Webcam ingest and Program continue advancing; restored original
Program routing is retained. Recording/streaming are off and the original saved
virtual-camera enabled state is restored. No public beta was published. Independent
full-workload/display/stuttering qualification and #1009 remain open.
