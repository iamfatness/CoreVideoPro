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
