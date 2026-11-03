# Graphics Designer Slice 5 implementation plan

Use the Slice 5 worktree and commit each task after recording a failing test, implementing the change, passing its focused tests and reviewing the diff.

1. **Names-list model and CSV.** Extend persisted variants with bounded extra cells and default old variants to no extra cells. Test quoted values, empty cells, stable ids, legacy syntax and round trips.
2. **Sources columns and persistence.** Add bounded column definitions and slot cells to show data. Test restart and migration, identifier stability, validation and slot isolation.
3. **Playlist mapping and playout.** Store per-row mapping and retain compatible mappings on re-pin. Resolve typed values against the exact template in the existing cue preparation path. Test guests, variants, malformed input and fallback. Preserve current name and title behavior.
4. **Operator controls.** Add the Designer column offer, Sources editing and Operate mapping. Use the pinned variable schema for names-list imports and editing. Keep draft and on-air state separate. Extend the isolated layout probe.
5. **Acceptance and docs.** Trace both paths through Save, restart and Show. Run shell suites and inspect the 1360×768 and 1600×900 probe screenshots. Update the backlog, CLAUDE.md and graphics playout reference, then open a PR. Record any installed-build checks still outstanding.

Keep evidence under D:\CoreVideo-QA\tmp. Use synthetic show data and preserve the installed app and owner show files.
