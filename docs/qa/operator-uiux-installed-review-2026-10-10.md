# Installed operator review — October 10

Baseline: installed `beta-2026-10-10-abb97fb`, commit
`abb97fb89fd75527a1c2e425f72e0135877592d8`. Windows, main window approximately
1425 × 893 pixels. Zoom idle, five saved inputs offline, zero guests. Existing
shell/core processes remained running; webcam reported live at 1920 × 1080.
No meeting, stream or recording was started during this review.

This is partial evidence for the [acceptance protocol](operator-uiux-installed-acceptance.md),
not complete issue acceptance or a sustained output qualification.

| Issue | Result and limits |
|---|---|
| #594 | PASS: installed Settings expands/collapses shared Health, without duplicate Health/Diagnose navigation. Actual export produced a readable seven-entry ZIP. Local scan found zero matches for known stored plaintext credentials and zero bare JWTs; this is a bounded redaction check, not proof against every possible private value. Keyboard, detached comparison and physical narrow/high-DPI review remain MISSING_EVIDENCE. |
| #595 | PASS: enabling OHG saves the opt-in and reveals its tab while retaining Settings selection. Opening it displays the unconfigured workspace. Returned to Settings and restored opt-in off, hiding the tab. Restart, disable while selected, hidden-selection guard and existing configured-show preservation remain MISSING_EVIDENCE. |
| #590 / #593 | PASS: installed Sources shows assignment, Format/Rec and Options without duplicate feed-health controls. Source text flyout exposes default-secondary behavior and previews a deliberately long synthetic draft on readable wrapped lines; Escape dismisses without Apply. Live roster refresh, identity replacement, restart persistence and actual graphic output remain MISSING_EVIDENCE. |
| #847 / #848 | PASS: installed designer exposes sliders with exact entries, units/ranges/defaults, reset, and explicit draft/Apply/Cancel language. Selecting minimal-accent changes controls and the native sample preview; Cancel restores the original 42 px name size and 90% background opacity. No live Apply or named-look save was performed. Full typography/logo/anchor matrix, actual routed outputs and restart persistence remain MISSING_EVIDENCE. |
| #835 / #853 | PASS: fresh installed isolated grade/native probe completed all 12 checks, including scopes, original tap, ROI native sampling/circle mask, full-frame reset, Apply acknowledgement and preset/undo integration. Physical rectangle/circle drawing, real subject scopes, source churn, high-DPI input and receiver isolation remain MISSING_EVIDENCE. |
| #591 / #592 / #839 | MISSING_EVIDENCE: this idle-source review does not establish real starter-scene routing, metadata/casing stability, or webcam-only overlay/LUFS behavior. |

Fresh installed isolated operator probe passed all 13 checks: first-click field
validation, Health layout, default-off OHG, source edit identity guards, mixed
ten-row layout, atomic lower-third edits, exact slider precision/invalid-input
handling and designer loading. These compiled UI checks supplement physical review;
they do not replace the missing production/receiver criteria above.

Private evidence remains under ignored `artifacts/uiux-acceptance-20261010`:
`operator-probe.json`, `grade-probe.json`, `support-check.json`, redacted/limited
accessibility observations and preferences comparisons. Known encrypted secrets
were compared after correct DPAPI decoding without printing values; all three
remained semantically equal. Unrelated non-secret preferences were unchanged.
Source and appearance drafts were canceled; OHG was restored off; Studio was restored.

The fresh control snapshot repeatedly reports inactive recording as
`stopping` / `finalizing`, with queue depth 10 and zero frames/bytes/elapsed time,
while the operator footer says Idle. This persisted through 11:28–11:37 EDT.
Tracked in [#859](https://github.com/iamfatness/CoreVideoPro/issues/859).
No recording was started in this pass; a hung real writer or damaged file is not
established. The unconditional native stop-state transition is an investigation
candidate, not a confirmed root cause.

No UI/UX issue is closed by this partial pass. Actual live source/output acceptance
and the owner's weekend bake remain separate, with zero-miss criteria unchanged.
