# Graphics Phase 3 Slice 4 — data-aware Shot Layouts

Tracks [#1106](https://github.com/iamfatness/CoreVideoPro/issues/1106). Extends the [Phase 3 design](2026-11-03-graphics-phase3-data-production.md) and the existing Shot Layouts in Graphics Operate. Local CSV and JSON are sufficient to ship this slice. Remote sources use the same stored revision contract when Slice 3 is present.

## Operator workflow

In **Graphics → Operate → Edit layouts**, pick a scene and the playlist rows that should ride its Take. Expand a row to set a typed field override or choose its dataset revision. Save the layout independently of the playlist. A row stays reusable across multiple layouts and can still be shown directly with its ordinary playlist values.

Cue runs preflight and stages the layout on Preview. Show a compact list of the scene and required rows with their revision and status. Use **Take** once all items are ready. Show **Cancel** while preparing or ready. A missing scene, row, template, source binding, data revision, invalid field or resource refusal identifies the affected item and blocks Take. An operator can cancel, fix the item and Cue again. A status that needs reconciliation is distinct from a definite refusal.

## Persisted selections

Keep `RowIds` as the ordered list. Add an optional row-settings map keyed by those IDs to the current show document. Only listed rows can have settings. Reject duplicate IDs or field keys, empty or malformed settings, and oversized or non-scalar override values. Use the existing 16-row limit and template field limits. Saves through the editor and existing control API share validation. Old documents with no settings keep their current behavior, including legacy Clean and Shift+F1 through Shift+F11. Shift+F12 remains Play Out All. Existing `graphics.layout.*` action IDs and arguments stay valid.

⚑ Override declared string, number, boolean and color variables with their existing template constraints, including allowed choices, length and numeric bounds. Apply them after normal row, source and variant mappings. A row-level override applies to every resolved entry of that row. Native data payload fields, engine-owned clock or generated values, image references and template/style controls are excluded. Validate against the exact template version in the resolved cue. An incompatible template edit makes the override visibly invalid. Refuse it instead of dropping it or converting its type.

⚑ A data-bound row defaults to **Use published data**. Also offer **Pin revision N**, storing the source ID and exact revision. A pin can resolve to the row's persisted published fields or to the latest accepted dataset for that source, materialized using the existing table/crawl projection. Capture either as immutable cue values. If the source changes, the requested revision is no longer retained, or projection fails, refuse the layout and identify the row. This slice adds no revision archive. A refresh after Cue leaves its captured values unchanged. An available stale revision can be cued with a visible age or stale warning. Choosing data for a layout does not publish it to the playlist. After the initial Take, existing manual publication and opt-in live follow rules apply.

## Preparation and Take

Preflight captures the show identity, effective Preview target, scene revision, layout contents, exact row/variant IDs, template pins, value overrides and selected data before cue mutations. Validate references, inputs and known limits up front. Native preparation is the final authority for admission and resource readiness. Reuse the existing revisioned cue preparation path. Assign a unique attempt identity to a layout with row settings, and keep its cues private until a guarded Take. A replaced cue, changed Preview target, core restart, show switch or cancellation invalidates the attempt. A subsequent fetch alone does not invalidate an already prepared data snapshot. Release only cues still owned by the attempt. Preserve unrelated cues and any newer Preview edits.

⚑ The ordinary Take transport carries the attempt identity and the expected scene and cue identities for a data-aware layout. Native validates the complete group, current source bindings and prepared resources before committing any Program change. At the transition commit boundary, promote exactly that group and apply **Play out other graphics** there if selected. Keep unrelated private cues available for a later ordinary Take. Hold the old scene and graphics together while an incoming scene is held for cold media. A failure before commit preserves Program, including existing graphics and hide timers. A layout with **No scene change** keeps the current Program scene and routes. Pending layout readiness also gates the Studio Take entry point, so a manual or control Take cannot consume a partially prepared layout. Older layouts without settings follow their existing cue and Take path.

⚑ One guarded Take attempt has one stable operation ID. Retry after an uncertain reply reconciles the original operation against its core epoch and native disposition. An accepted attempt consumes its cues once. A known refusal can be prepared again explicitly. If the previous core is gone or the outcome cannot be established, show that confirmation is unavailable and require a fresh Cue before another Take. Cancel before submission releases preparation. Once submission is uncertain, reconcile it before offering another Take.

## Acceptance

- In an isolated show, save a layout for a different scene with a CSV leaderboard and JSON crawl. Override one declared field on each and pin an available revision. Cue, stage a newer dataset and Take. The selected scene and both prepared graphics appear on the first incoming frame with the captured values. The saved playlist values and bindings are unchanged.
- Repeat with an unavailable revision, a failed second preparation, a replaced cue and a pending cold scene. The first three cases preserve the existing Program. In the cold case, the outgoing scene and graphics stay together until the transition commits.
- Enable Play out other graphics and confirm those graphics remain visible through Cue and leave at the guarded Take. Cancel a different attempt and verify an unrelated private cue survives. Drop the reply after a successful Take and confirm retry does not toggle the scene or replay entrances.
- Exercise the same workflow through the editor, layout hotkey followed by Studio Take, and the existing control actions. Verify legacy layouts including Clean.

Audio actions, arbitrary macros, data editing, provider-specific policy and revision archives are separate work.
