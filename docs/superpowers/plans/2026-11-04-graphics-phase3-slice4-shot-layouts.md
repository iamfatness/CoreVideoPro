# Graphics Phase 3 Slice 4 — implementation plan

Build [#1106](https://github.com/iamfatness/CoreVideoPro/issues/1106) against the [slice spec](../specs/2026-11-04-graphics-phase3-slice4-shot-layouts.md). Base implementation on main after reviewing current dependencies. Use local data for the baseline so Slice 3 can qualify independently. Work in order. For each task, record a reproducible red test, implement, run it green, review the diff against this plan and commit.

## 1. Store and resolve layout selections

Start in GraphicsDocumentStoreTests and GraphicsPlayoutServiceTests. Round-trip a two-row layout and open a legacy show with no new fields. Add cases for orphan or duplicate keys, wrong field types, prohibited generated fields, re-pinned template schemas, a missing revision and a rebound source ID. Assert the invalid second row causes no scene, cue, hide or Take calls. Verify that pinned published fields and latest accepted data can both resolve, including a stale-but-usable snapshot.

Extend ShotLayout and the GraphicsDocumentStore / GraphicsWorkspaceLibrary.Playlist validators. Add a focused layout resolver beside GraphicsPlayoutService that takes a captured show document and read-only scene and core context. Reuse GraphicsRowFields.TemplateFieldsOf, the existing mappings and GraphicsDataTable.Fields / GraphicsCrawlData.Fields. Separate read-only entry resolution from instance preparation so no first-row mutation happens before the last-row reference is checked. Copy fields into immutable attempt inputs and keep the playlist untouched. Validate template field limits with the existing native semantics. Preserve the normal row and legacy layout paths.

Commit **Resolve typed Shot Layout values and pinned data**.

## 2. Prepare and cancel a guarded layout

Start with a controllable fake facade and injected clock. Hold the second preparation pending, fail it, then complete it late after Cancel. Assert the layout stays off Program and the late completion cannot arm Take. Add cases for an unrelated Preview cue, a replaced cue, a changed Preview scene, and a feed refresh or publish during preparation. Preserve the layout's captured values and any independent cue.

Extend the existing Ensure and Cue path in GraphicsPlayoutService for resolved inputs and attempt-scoped cue ownership. Capture exact native identities from ready observations, including epoch, instance, cue revision and attempt ownership. Use an aggregate deadline on the existing injected wait seam. Keep the pending layout's Take gate and cancellable lifetime in a focused coordinator; Cancel must remain reachable during native preparation. Fence all cleanup by ownership and epoch, including after a lost cue response. Separate scene existence checking from SelectSceneForLayoutAsync, and use the existing Preview scene path to stage the resolved scene. Guard against intervening edits through the Preview revision. Carry PlayOutOthers as Take intent; never call Hide on Program while preparing this path. Test that legacy Clean retains its current behavior.

Commit **Prepare and cancel guarded Shot Layout cues**.

## 3. Commit the group on the ordinary Take

Write exact-filter native tests in GraphicsInstancesTest and MediaCoreCommandTest, plus a ProgramPixelContinuityTest or D3D11 pixel fixture for the transition boundary. Start with a valid first cue and one invalid second cue. Require zero Program changes and zero Play Out side effects. Add a cold incoming scene, an unrelated cue, duplicate operation delivery, and a stale Preview or cue revision. Exercise a guarded Take with no scene change. Use the native tick or injected clock for all timing.

Follow TransportCoordinator.TakeAsync through the production sync context and MediaCoreCommandBuilder.BuildTakeTransitionCommand into MediaCore::beginTakeTransition and loadSceneGraph. Today beginTakeTransition promotes all ready rideTake cues, and a subsequent scene load can hold for cold media. Carry an optional guard through the existing Take context. Validate the guarded scene and entire cue group before accepting the scene mutation, and recheck at the actual commit boundary if the scene was held. Preserve existing cold-media policy, with guarded graphics held alongside the outgoing picture. Use the same show, clear and hide primitives in GraphicsInstances for scoped promotion and Play Out, publishing the state together. Enforce the guard over the full scene-load path, including subsequent commands in the production batch; rejecting only the initial transition command is insufficient. Leave legacy unguarded batches compatible. Use a bounded epoch-scoped receipt with operation ID and disposition so a duplicate can be answered without applying mutations twice. Thread the receipt through the existing observation/command contract and run contract parity. Keep new coordination in focused code.

In TransportCoordinatorTests, drop the reply after native acceptance. Verify retry reuses the pending operation and reconciles before any new swap. Cover a definite refusal and a core epoch change. Wire normal Studio Take, layout Take and control entry points through the same readiness gate and result. Confirm rejection restores the shell's local scene state with the existing rollback fencing and preserves newer edits.

Commit **Guard Shot Layout scene and graphics on Take**.

## 4. Expose the workflow and qualify it

Start with a red WinUI layout probe for per-row settings, preflight status, and Cancel while preparing. Extend GraphicsPlayoutControl's existing editor and layout header, plus the existing view-model partial and control routing. Preserve action IDs and hotkeys, keep failures in the dialog, keep status and Cancel visible while waiting, and avoid unhandled exceptions in WinUI handlers. Show a ready summary and explicit blocked or reconciling status. Keep save, cue, cancel and take state distinct.

Run the MediaCore, Control and WinUI suites. Build with npm run build:native-dev using the local SDK setup. Run exact native filters from native/build-dev with COREVIDEO_VCAM_SHM_DIR isolated under D:/CoreVideo-QA/tmp. Run the full native suite once at the end only after confirming the installed app is closed. Run npm run contract:check and use a fresh report directory for the Studio layout probe. Read the 1360×768 and 1600×900 screenshots and capture 1080p where available. Record any baseline failure by exact name and evidence. Exercise the spec acceptance with isolated synthetic data. Keep the installed app, owner show files and OAuth broker configuration untouched. Keep all evidence under D:/CoreVideo-QA/tmp.

Update CLAUDE.md and docs/reference/graphics-playout.md. Review the whole branch against the acceptance cases and fix findings. Open an implementation PR with red/green evidence, suite counts, flags and installed checks outstanding.

Commit **Expose and qualify data-aware Shot Layouts**.
