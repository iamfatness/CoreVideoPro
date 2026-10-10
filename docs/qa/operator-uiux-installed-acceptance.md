# Installed operator UI/UX acceptance protocol

Review protocol for the owner-approved editing/navigation bundle, tracked under
#848 and the issues linked below. This is a test protocol, not a ranked work queue
or a second status ledger. `docs/BACKLOG.md` defines order; results live on issues.

## Baseline and evidence rules

Baseline: published/installed [beta-2026-10-10-abb97fb](https://github.com/iamfatness/CoreVideoPro/releases/tag/beta-2026-10-10-abb97fb),
commit `abb97fb89fd75527a1c2e425f72e0135877592d8`. PRs #837, #840, #842–#846,
#849 and #855 are merged. Open issue status does not mean their implementation
must be repeated. Read the issue's complete acceptance criteria before closing it.

Record beta/commit, hardware, display scale, window size, source mix, duration,
action, expected/observed result and redacted evidence paths. Report `PASS`, `FAIL`
or `MISSING_EVIDENCE` for each tested criterion on its issue. A partial pass never
closes an issue with remaining criteria. Preserve failed trials. UI values and
advancing publisher counters do not prove receiver pixels, sound or continuity.

Back up preferences before edits. Run fresh-profile/migration checks in an isolated
profile; do not replace the owner's saved show. Use authorized test meetings and
local receivers. Preserve unrelated live sessions. Do not start a public stream,
rejoin a meeting or restart an active show merely to follow this document. Restore
test edits and retain unrelated settings. Redact credentials and meeting secrets.

Registration and receiver activation are prerequisites when webcam comparisons
are needed. #857 remains unresolved despite restart recovery; #856 remains an
unattributed helper exit. If either recurs, retain incident evidence and mark the
affected checks failed or unavailable instead of treating a reconnect as a pass.

## Short installed review

Each row is a procedure and expected result, not a completion checkbox. Use real
Zoom, capture and media sources where the criterion calls for them. The desktop
shortcut and its actual launch path must be tested explicitly; direct executable
launch does not establish shortcut acceptance.

| Issue | Exercise | Required result/evidence |
|---|---|---|
| [#847](https://github.com/iamfatness/CoreVideoPro/issues/847) | Review compact, minimal and broadcast presets; change typography, colors, opacity, spacing, corners, anchor/offset and transparent logo. Use short/long names, blank secondary text, missing font/image and narrow/high-DPI windows. Compare draft/applied, Cancel, Apply, save/duplicate/reset and restart. | Draft/Cancel leave on-air pixels unchanged. Apply publishes one complete look without resetting key/text or replaying build. Supported fields visibly affect native output; text stays inside bounds, blank lines collapse, logo aspect/alpha and fallback feedback are correct. Named looks and unrelated settings survive restart. |
| [#592](https://github.com/iamfatness/CoreVideoPro/issues/592), [#593](https://github.com/iamfatness/CoreVideoPro/issues/593) | Exercise source default, authored name/secondary, deliberate blank and reset. Refresh metadata, switch scene/source, replace a guest in a slot, reopen an old editor and restart. Trace native key/content and capture the actual graphic through refresh and Apply. | Authored text and blank/default behavior persist by source identity. Recycled/stale editors cannot change another source. Refresh does not alternate guest/Guest or replay transitions. Genuine edits update once; inspect output for clipping/partial glyphs, not only sampled metadata. |
| [#848](https://github.com/iamfatness/CoreVideoPro/issues/848) | Review implemented grading, scene/framing, audio/mastering and appearance adjustments. Drag sliders, type precise/invalid/out-of-range values, use keyboard/reset, scroll an unfocused page and compare saved values after restart. | Slider/value entry share one setting without precision loss. Units/ranges/defaults describe the native effect. Invalid entry preserves the prior value; reset is scoped. Unfocused wheel input does not change values. Live/draft behavior is explicit; real pixels/PCM or processor feedback confirm effects. Any unconverted/confusing control is a remaining finding, not silently accepted. |
| [#835](https://github.com/iamfatness/CoreVideoPro/issues/835), [#853](https://github.com/iamfatness/CoreVideoPro/issues/853) | On a known-color subject/background, compare full-frame and Original/Graded scopes. Draw rectangle/circle in both directions; redraw, move/resize, use arrows/Shift+arrows, Escape/lost capture, Clear and collapsed precision overrides. Switch source, resolution/aspect and editor during pending analysis; reopen/restart. | Histogram/waveform/vectorscope include only selected source pixels; waveform spans the ROI. Circles remain circular in source pixels. Letterbox areas are excluded. Cancel restores prior geometry; Clear restores full frame. Pending/stale results cannot replace the current selection. Guides stay editor-only; full-source grade and output remain unchanged by ROI. Review readability, scope proportions and pointer/keyboard/high-DPI behavior physically. Preserve advanced grading/LUT/preset criteria on #835. |
| [#591](https://github.com/iamfatness/CoreVideoPro/issues/591) | With an isolated fresh profile, assign distinct inputs and choose solo, two-person and screen-plus-presenter looks. Repeat with fewer sources and an upgraded custom scene. Assign, preview, Take and adjust framing. | Starter positions use distinct appropriate inputs or clear empty placeholders; they never silently repeat the active speaker. Share/presenter routing is intentional. Existing custom routes/geometry survive. Actual Preview and Program agree with the assignments. |
| [#590](https://github.com/iamfatness/CoreVideoPro/issues/590) | Inspect ten mixed Zoom/capture/media input rows at normal/narrow widths; scroll and use keyboard. Open/edit Role and On dropout, reassign while an editor is open, refresh roster and restart. | Discovery, names, assignment, Format/Rec and Options remain reachable/readable. Refresh preserves focus/selection. Only the intended source receives edits; stale editors reject writes. Saved assignments/dropout policy persist and native behavior matches. Duplicate feed-health controls remain removed without losing diagnostics. |
| [#594](https://github.com/iamfatness/CoreVideoPro/issues/594) | Navigate Settings by keyboard, expand/collapse Health, watch live state changes and export/open a support bundle. Compare shared and detached diagnostics at normal/narrow widths. | Health/support remain accessible and refresh correctly. Export produces readable redacted evidence with no credential leakage. Navigation/focus and resizing are usable; Diagnose/Health are not redundant top-level entries. |
| [#595](https://github.com/iamfatness/CoreVideoPro/issues/595) | Check fresh-profile default, enable OHG, select it, disable while selected, attempt hidden-tab selection and restart. Compare existing OHG configuration before/after. | Default is off; opt-in persists. Enable exposes the tab without unexpectedly selecting it. Disable/hidden selection returns to Settings. Existing OHG configuration and unrelated preferences remain intact. |
| [#839](https://github.com/iamfatness/CoreVideoPro/issues/839) | Enable/disable webcam-only framer and LUFS-S overlay with known video/audio; compare camera receiver against clean outputs, then restart. | Framer alpha passes video through, LUFS-S follows the intended Program audio measurement, unavailable audio is explicit, and camera preferences persist. Guides/meter affect webcam only; Program, Preview, recording and stream stay clean. |

## Output comparison and sustained qualification

For appearance, source text and grading, compare actual Preview/Program, a webcam
receiver, decoded finalized recording and a local stream receiver. For audio, listen
to/capture the intended routed bus and check the processor's native response. Record
which paths were observed; unavailable outputs remain `MISSING_EVIDENCE`. ROI guides
are absent from every output; the framer/LUFS overlay is exclusive to webcam.

The separate weekend bake uses real guests, Takes, lower-third edits, scopes/ROI,
webcam, recording/ISO and the intended stream. Preserve exact workload and separate
render overruns from Program/output missed slots. Compare scopes/editing on versus
the same baseline workload. Any missed scheduled delivery slot fails the applicable
#517/#823 performance criterion; smooth observation or average FPS cannot waive it.
Verify final files, audio/video sync, source churn, helper lifecycle and camera
relaunch. No duration alone establishes all criteria or fleet qualification.

## Design and historical evidence

- [Bundle specification](../reference/operator-editing-bundle-spec.md) and [implementation plan](../reference/operator-editing-bundle-plan.md).
- [Adjustment control specification](../reference/operator-adjustment-controls-spec.md), [grading specification](../reference/source-grading-spec.md), [drawn ROI contract](../reference/roi-drawing-tools.md), [webcam framer specification](../reference/webcam-framer.md).
- [Combined validation](../reference/operator-editing-validation.md), [Sources audit](sources-controls-2026-10-09.md), [source text](source-lower-third-text-2026-10-09.md), [casing investigation](lower-third-casing-2026-10-09.md), [Settings/Health](settings-health-2026-10-09.md), [OHG opt-in](ohg-navigation-opt-in-2026-10-09.md).

Original branch evidence remains historical. Store new acceptance results on the
linked issues with beta identity; do not rewrite an old trial as a current pass.
