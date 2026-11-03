# Graphics Designer Slice 4 implementation plan

Work in order. For each task record the failing test, implement, rerun the affected cases and review the diff before committing. Use the separate Slice 4 worktree and keep Slice 3 reviewable.

1. **Native shapes.** Test ellipse and line validation and resolved primitive geometry. Add inset strokes and D3D pixel tests for ellipse coverage, line orientation and rounded-rectangle strokes. Check native thumbnails and layered motion use the same primitives.
2. **Native text effects.** Validate optional outline and shadow, test limits and draw order, and verify D3D pixels and legacy text parity. Keep preparation cost and texture extents bounded.
3. **Native size to content.** Test field-dependent widths, clamp bounds, cyclic rejection, shrink before ellipsis, and fixed-size parity. Use the same measured boxes for drawing, authoring geometry and motion.
4. **Design assets.** Test PNG/JPEG and SVG import, validation, hashing, transparency, versioned retention and safe eviction. Implement one import route for design images and the logo picker. Exercise draft, Save, reinstall and cue paths.
5. **Designer controls.** Add shape choices and inspector edits, wire sizing and text effects, and surface import results. Keep edits private and undoable. Exercise the controls at both viewport sizes in the isolated layout probe.
6. **Acceptance and docs.** Trace SVG import through the asset lease, draft, saved pin, Operate and native Program. Run shell suites and the native dev build, focused tests and one full native run with the installed app closed. Review screenshots, update operator docs and report live verification gaps.

Use injected clocks for motion. Keep logs and probe images under D:\CoreVideo-QA\tmp. Preserve the installed app and the owner's show files.
