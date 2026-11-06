# Recording identity through source loss

Issue: [#1188](https://github.com/iamfatness/CoreVideoPro/issues/1188). Ordering lives in BACKLOG.

The retained local rehearsal lost all Zoom participants, then stopped the stream before stopping recording. The shell rebuilt its eligible ISO selection as empty, and the next production sync opened a Program-only take. That empty take failed finalization and replaced the original take's final status.

## Contract

Passive roster loss, camera-off, or temporary source unavailability must not restart an active recording or replace its finalization evidence. A take begins with eligible selected sources. Preserve their identities during that take when they temporarily disappear; native departure policy remains responsible for stopping their media. Do not add unavailable sources at start or resurrect departed media. Explicit ISO deselection, unassignment, master disable and recording stop remain operator actions. A subsequent take resolves eligibility afresh.

## Execution and acceptance

Reproduce through shell selection and command building: start with selected video sources, remove the roster, send an unrelated stream-off sync, then record-off. The recording session identity stays constant until stop. Cover partial departure, video-off, reappearance, deliberate selection changes, stop/new take and idle eligibility. Retain the failing old-code control.

Use a focused state owner rather than adding behavior to StudioViewModel.cs. Preserve existing ShowInputs and recording command regressions. Rehearse the original command ordering in the packaged app with synthetic sources, retain the same session through stream stop, then require finalization of its original files and no extra folder. Combine with the separate final-picture check when practical, judging each independently. All exact-head hosted checks must pass before merge. A short stop regression is not full output qualification.
