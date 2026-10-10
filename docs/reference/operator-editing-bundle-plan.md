# Operator editing bundle implementation plan

Implementation plan for the owner's combined lower-third, grading ROI, scene,
audio and control-usability request. The [bundle specification](operator-editing-bundle-spec.md)
defines behavior. `docs/BACKLOG.md` is the only ranked list; GitHub issues carry
status. This is a delivery/dependency plan, not a competing queue.

## Delivery baseline

As of October 10, 2026, grading #837, webcam framer #840, Settings/Health #842,
OHG opt-in #843, Sources #844, source text #845 and casing repair #846 are merged.
The appearance/shared-control/starter-scene bundle #849 and draw-first ROI #855
are also merged. Their combined implementation is published and installed in
[beta-2026-10-10-abb97fb](https://github.com/iamfatness/CoreVideoPro/releases/tag/beta-2026-10-10-abb97fb),
source commit `abb97fb89fd75527a1c2e425f72e0135877592d8`.

These merges establish implementation, not complete installed acceptance. The
remaining checks are consolidated in the [installed UI/UX acceptance protocol](../qa/operator-uiux-installed-acceptance.md).
Use it with each issue's acceptance criteria; do not rebuild the shipped features
merely because an issue remains open. Earlier mapping-only comparisons and draft
probe evidence remain historical, with their original limits preserved.

## Delivery boundaries

| Work package | Implementation and dependency | Reviewable result |
|---|---|---|
| Control inventory and design (#848) | Map targeted controls to native parameters, existing ranges/defaults, live/draft policy and saved values. Define display conversions before changing widgets. Reuse native controls and establish the slider/value behavior through a real consumer. | Interactive rendering of representative lower-third, grade, scene and audio controls, including precision entry and reset |
| Lower-third text and stability (#592/#593) | Finish frame/content tracing and output acceptance of existing fixes. Validate intentional blank/default behavior and real edits. Keep each issue's patch separate. | Stable source-bound key and persisted text with recorded output evidence |
| Lower-third appearance (#847) | Add versioned style/preset contract and native rendering consumer, then designer/preview. Share immutable raster/cache path and atomic content/style revision. Depends on the shared value interaction; integrates text behavior above. | Three rendered presets and working visual customization in the installed app |
| Scope ROI (#835) | Extend analysis request/result contracts with geometry/revision; implement region sampling and known-patch tests; add monitor selection/handles/precision fields and stale-result rejection. Uses the existing native grade/analysis path. | All three scopes respond only to the selected region; whole-source grading remains unchanged |
| Scene setup (#591) | Reproduce first; fix starter input bindings and preserve custom scenes. Apply shared controls to existing geometry/framing UI rather than adding a second canvas. | Useful starter looks and predictable direct/precise adjustment |
| Audio controls (#848) | Apply shared controls to confusing processing parameters after auditing native units. Preserve existing mixer/DSP behavior and saved values. | Understandable processing controls with real response/meter feedback |
| Combined review candidate | Integrate accepted issue branches with grading/framer and setup/navigation changes. Resolve stack dependencies, run current-head checks and package/install verification. | One coherent candidate and consolidated acceptance evidence |

Respect the repository's three-item WIP limit. Finish or integrate active items
before opening more implementation lanes. Keep one issue per focused PR; dependent
PRs may stack, but the owner reviews the combined behavior in one candidate.

## Verification approach

Run focused contract/parameter/identity tests for each change, followed by actual
compiled UI checks. For ROI, independent expected values from known patches must
exercise boundaries, tiny regions, Original/Graded and waveform horizontal mapping.
Use fresh and upgraded profiles for scenes and persistence. Preserve source identity
and existing numeric settings through all migrations.

Short installed tests use the authorized test meeting and measured native facts.
Check actual output pixels/audio, finalized files and a local stream receiver where
applicable; UI values alone are not evidence. Test rapid adjustment, Apply/Cancel,
source switching, unavailable/stale analysis and close/reopen resource release.
Compare production delivery with optional scopes/editing enabled against the same
baseline workload. Retain failed trials and label missing evidence explicitly.

The weekend bake uses the combined candidate: real Zoom sources, scene changes,
lower-third edits, scope ROI, webcam, recording and the intended stream. Record
continuity, final-file and lifecycle evidence against the relevant issue criteria.
Do not close long-run acceptance from short checks, or publish a public beta from
an unmerged integration branch. Release requires the applicable current-main gates.

## Review handoff

Use the installed beta to review the three lower-third looks, shared controls,
ROI gestures, starter scenes and navigation under the consolidated acceptance
protocol. Record results and evidence on each linked issue. Finish the short
checks before requesting the separate sustained meeting bake; neither can replace
the other. `docs/BACKLOG.md` remains the work order.
