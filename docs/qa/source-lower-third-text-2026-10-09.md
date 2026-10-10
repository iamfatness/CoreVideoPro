# Source lower-third text — #593

Eligible Zoom/capture source rows now have a Lower-third text editor beneath the
existing name field. The editor shows Name, Secondary line, a source-default
checkbox, reset-name action, and a text preview. Draft edits update the preview;
Apply commits both values in one preference save and one lower-third refresh.
Escape/light dismiss cancels drafts. Existing inline name editing remains available.

Secondary-line storage is additive to production preferences:

| Saved state | Result |
|---|---|
| No canonical source key | Existing source metadata defaults |
| Nonempty value | Custom text, with the legacy organization/role suffix suppressed |
| Empty value | Hide the whole second line; no title or organization fallback |
| Use source default on Apply | Remove the override and restore defaults |

Name uses the existing override/default behavior. Reset removes the name override.
Text is keyed to the canonical source identity, never the Show Input slot number.
Guest replacement therefore does not inherit another guest's text. Capture IDs
are persistent; Zoom SDK participant IDs are meeting-scoped. This does not claim
identity matching across separate meetings with new participant IDs.

The existing native-rendered-source resolver still determines which source owns
Program's automatic lower third. Its name/title/org now pass through the focused
operator-text helper. An on-air same-source edit uses the existing in-place key
refresh, without forcing a build-out/build-in. Media assets remain outside that
automatic keyer's source eligibility; their inline display-name control is retained
and the new lower-third action is hidden. This is not a new media keying feature.

An open editor captures its source ID and editor object. Apply rejects reassignment
or a recycled owner Tag. The preview represents the text content; it is not an
exact native graphic/style rendering.

## Evidence and limits

Release WinUI build and all 1,610 WinUI tests pass. New tests cover explicit blank
versus reset, metadata changes while hidden, idempotent writes, real file-store
restart with blank/custom values and unrelated output settings, and replacement/
return of guests in one slot. The real compiled-XAML probe covers the editor's
draft preview, one atomic Apply callback and rejection after replacement, alongside
the existing ten-row template at 900/1,100 pixels. The first probe's preview assertion
ran before WinUI dispatched its queued text event; that report is retained and the
corrected probe awaits dispatched state before checking it.

Reports remain in operator-ux-587/artifacts/lower-third-text-*. The probe owns an
offscreen process, does not join Zoom or change the installed user app.

Installed acceptance remains: source-default/custom/blank/reset text, scene changes,
reassignment, restart persistence, actual Preview/Program and recording or stream
pixels, and an on-air edit without a spurious transition. The text preview and
unit tests do not establish those output checks. #592's observed refresh/lowercase
fragment still needs its own frame sequence and trace; this change does not assert
that its root cause has been found or fixed. Those statements describe this
original trial; the separate casing investigation subsequently established the
guest/Guest mapping cause and shipped in #846. PR #845 is also merged and both
are included in installed `beta-2026-10-10-abb97fb`. #593 remains open pending
acceptance under the [UI/UX acceptance protocol](operator-uiux-installed-acceptance.md).
