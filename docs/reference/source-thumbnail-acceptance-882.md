# Sources thumbnail identity acceptance

Issue: [#882](https://github.com/iamfatness/CoreVideoPro/issues/882). Ordering lives only in BACKLOG.

The original report described adjacent pictures and shifted thumbnails after removing an input. The since-merged per-source publication contract (#899) removes the composite crop from this path, but its native fixtures and managed projection tests do not establish the real compiled Sources page's assignment lifecycle.

## Bounded implementation

Add an opt-in verification mode to the existing offscreen source-video host. Use a dedicated native core, in-memory operator preferences, three independently named 1920x1080 synthetic SHM capture producers and the actual StudioWorkspace, source coordinator, source rows and GPU presenter. Each producer changes intensity while retaining a distinct primary-color identity. No real meeting, hardware capture, installed profile or public output is involved.

Exercise actual source-picker unassign/reassign commands, assignment swaps, Sources navigation away/back and capture unregister/reregister. Preserve row/control identity and unrelated assignments. Observe source identity, native surface keys, natural dimensions, presentation progress and decoded presenter probe pixels. Retain each phase and failures in a structured report. Existing one-pixel QA readback stays opt-in; add no production CPU pixels, subscriptions, locks or presenter ABI.

## Evidence and limits

Every observed valid picture must match the row's current assigned identity. An unassigned row must hide/clear its thumbnail. A replacement may be pending/blank while acquiring its own surface, but must not present the old or another source under the new binding. Require multiple changing pixel values for each producer and actual GPU presentation progress. Deliberately bind an adjacent source in a negative control; the same identity assertion must fail, then restore the source and rerun.

This probe samples the existing presenter pixel seam. It cannot certify every pixel of every transitional frame or physical desktop presentation, so a passing result alone does not close #882's stronger acceptance. Retain that limitation alongside the native full-source isolation tests and any observed defect. If a real failure appears, reproduce its specific seam and repair a focused owner with a failing-before/passing-after regression; do not grow StudioViewModel or replace GPU delivery with CPU capture.

## Execution

Implement source-only while the independent output measurement runs. After it ends and the owned app closes, compile the real shell, execute the isolated probe, retain negative-control results, and run the relevant Sources/projection regressions. Review exact-head hosted checks before merging. Stop expanding this slice after 90 minutes of active work; preserve unresolved evidence and reforecast within the authorized work window.
