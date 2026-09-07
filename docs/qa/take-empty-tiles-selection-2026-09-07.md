# Take crash caused by an empty Tiles editor selection

At 2026-09-07 16:35:09 America/New_York, the local diagnostic shell (d97eeaa,
PID 106524) terminated with ArgumentNullException, parameter key. The managed
stack identifies Dictionary.TryGetValue -> TilesOverridePolicy.EditorValues ->
LoadGalleryTileEditor -> OnPreviewSceneIdChanged -> TransportCoordinator.TakeAsync.
Windows recorded a WinUI stowed exception (0xc000027b); the managed stack gives
the actionable cause. The local dump and logs were preserved privately.

The source dropdown refresh can clear its two-way SelectedValue. The view model
previously retained null in its non-nullable source ID property. A subsequent
Take swapped Program/Preview and hydrated the editor against a Tiles scene's
non-null override dictionary, causing the null-key lookup to throw.

Normalize a cleared binding to an empty ID and make EditorValues explicitly
accept nullable IDs. Empty/whitespace selection gets a fresh default override;
valid selections still clone their saved values. Do not swallow Take exceptions
or alter routing to mask this failure.

Release build and 17 targeted Tiles membership/editor tests passed, including
null/empty/whitespace refresh and preservation of existing overrides. Independent
review checked adjacent selected-member commands and found no additional uncaught
null-key path. Live replacement-build validation is pending.

The local replacement copy (shell 33e7c08, native 3371f00) joined the authorized
meeting and remained running with no repeat of the logged null-key exception.
Two bounded API selection/Take runs timed out on native rendered ownership;
neither passed. In the second run, API frame/buffer counters stopped advancing
while the retained core log continued to show render activity. This does not
by itself distinguish stale telemetry from delivery failure. Both failures were
preserved in private artifacts/take-null-fix-33e7c08/.

After explicit restoration, API confirmed both VM and native Program/Preview
were speaker-slides, rendered scene was speaker-slides, frame count had advanced
to 11597, auto-Take was restored, Zoom was live, and recording/streaming were off.
The new shell remained alive and responding. This is not a passed live Take
acceptance result; the null-key regression tests and subsequent timeout are
separate evidence. The diagnostic copy includes the complete pinned FFmpeg
runtime. No public package was published.
