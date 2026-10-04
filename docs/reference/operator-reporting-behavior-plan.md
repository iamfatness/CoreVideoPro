# Operator reporting, settings, automation and usability implementation plan

Owner direction: October 4, 2026. Baseline: main `92e6b1ea` and the published
`beta-2026-10-04-92e6b1e`. This document describes implementation and acceptance;
`docs/BACKLOG.md` remains the only work order and GitHub issues hold status.

## Outcome and boundaries

The operator can trust what the app says, see the effect of a setting, and run
the show without stale inputs, misleading controls or moving buttons. Complete
the existing issue scope below in the owner's sequence: reporting, behavior,
then usability. Existing installed acceptance can continue alongside this work.

Preserve resolution, configured frame rate, codec, bitrate and audio continuity.
Do not change output buffering, capture scheduling or recovery policies merely
to improve a status display. Shell code owns presentation and commands; the core
owns media and measured facts. Extract focused policies/coordinators rather than
adding new behavior to `StudioViewModel.cs` or a new ingest path to `MediaCore.cpp`.

No new licensing/caption backend, scene-canvas redesign, AV1 qualification or
new hardware ingest is included. Mock caption/licensing decisions remain under
#743/#767. Remove fabricated measurements now even if those panels are removed
later. Do not build a caption service just to fill a metric.

## Establish current behavior before each patch

Each issue's original audit describes a historical baseline, not necessarily
the current defect. Reproduce the remaining behavior on current main and trace
the command, core state, generated observation, shell policy and displayed result.
Record what is already fixed on that issue; change only the remaining gap.

For example, #772 already separates `starting` from `live` in the state mapper
for #759. `NativeMediaCoreAutoProduction`, its slot bindings and the shell's use
of `LastSnapshot.AutoProduction` also exist for #761. Their existence does not
prove stream-only lifecycle coverage or real automation execution. Test those
consumers before adding another mapping or rewriting the director.

Use `contracts/observation.schema.json` and its generated readers for observation
changes. Extend the existing read models; do not hand-write another independent
snapshot projection. Keep evidence generation off the render hot path and bound
diagnostic storage. Distinguish an unknown value from a measured zero.

## Accurate output and health reporting

| Issues | Implementation | Acceptance |
|---|---|---|
| #759 | Complete current-core lifecycle emission for idle, stream-only and recording sessions. Make transport, output details and close guards consume the same lifecycle policy. Remove reachable current-core legacy fallbacks; preserve any intentionally supported older-core behavior explicitly. | Starting never reads Live. Fresh writer progress, interruption, retry, failure, stopping and finalization agree across surfaces. Stream-only and first-ever output sessions exercise the current path. A rejected start cannot latch live state. |
| #610 | Project existing per-destination backpressure into a compact scalar readout using `appliedDivisor`. Show effective feed cadence from configured Program FPS divided by that divisor, with the degradation reason and recovery. | Controlled slow-destination tests show the applied cadence at multiple Program rates. Recovery clears degradation only when the observed state recovers. No snapshot-rate collection rebuild or moving transport control. Effective feed cadence is not labeled as measured receiver FPS. |
| #519 | Replace estimated bytes and fixed latency with facts whose measurement boundary is named. Pipe bytes accepted by FFmpeg are distinct from network bytes delivered. Publish unavailable for end-to-end latency unless a receiver measurement exists. Derive rates from monotonic time and counters scoped to the current run. | Known payload size and interval produce correct counts/rates. Retry resets or changes run identity without negative/spiking deltas. Snapshots and bundles contain no unexplained `2100 ms` or bitrate-based byte estimate presented as measurement. |
| #765 | Inventory network quality, audio meters, caption metrics and the LIVE timer. Use real SDK events/PCM evidence where available; remove invented quality/confidence/latency values otherwise. Base elapsed time on monotonic output-session time, not poll count. State timer semantics for reconnects. | Timer tracks real elapsed duration regardless of polling cadence or capture state. Silence does not show fabricated audio activity. Unknown network/caption evidence stays unknown. Retired callbacks cannot overwrite current-run facts. |
| #551 | Publish independent ISO writer completion counters from worker evidence with an explicit accepted-versus-muxed distinction. Obtain byte size off the real-time path if supported; otherwise mark it unavailable until finalization. | Async independent writers show advancing genuine progress mid-recording. Dropped/queued frames are not counted as completed writes. Finalized file and published counts agree within the stated semantics. No file-stat work on every render tick. |
| #758, #562 | Retire bridged shared-memory registration and cached pixels on reader stop/disconnect. Allow native UVC fallback for vendor-named devices when their dedicated adapter is unavailable. Propagate cached adapter liveness separately from frame-change cadence, then enable capture dropout policy only for supported kinds. | Unplug/disconnect clears or holds the image according to the chosen policy, with truthful loss-of-signal status. A static healthy screen/browser, still or deliberately paused clip never drops solely for reusing a frame ID. Reconnect rejects late arrivals from the previous session and restores the current source. |
| #766 | Audit first-frame claims, roster revision/epoch handling, capture counts, render/delivery facts, empty bundle sections and contradictory PCM warnings against current code. Bind actual evidence or omit unsupported claims. Keep bounded operator/event history with redaction if exposed. | Joining a camera-off participant cannot prove a video frame. Program progress cannot prove Preview delivery. Roster restart/revision-zero transitions are explicit. UI, control snapshot and support bundle agree, and mixed PCM is not simultaneously reported as absent. |

Output vocabulary must separate operator intent, connecting, verified local
production, degradation/interruption and failure. “Receiver playback verified”
requires receiver evidence; local production alone cannot establish it. Retain
actionable errors in Details/Health even when the main status is concise.

Dependency: settle lifecycle semantics before the degraded readout and status
layout; settle adapter liveness before restoring capture dropout controls. The
diagnostic audit follows the corrected facts rather than inventing a new source
of truth. Each correction ships with its first real UI/bundle consumer.

## Settings and automation with observable effects

For each setting, trace UI edit -> persisted preference -> serialized command ->
core reader -> media effect -> observed state. An echoed command is not proof
that the setting was applied. Publish named unsupported/rejected behavior and
retain the operator's preference without silently claiming success.

| Issues | Implementation | Acceptance |
|---|---|---|
| #764: picture/brand/graphics | Audit LUT presets, logo asset paths, logo text, lower-third/caption styles, default-overlay behavior and image payloads. Implement effects within the existing GPU/overlay paths where supported. Hide or explicitly disable controls whose effect is unsupported; do not silently drop fields. Keep file decoding and asset preparation off the render thread. | Before/after pixels in Preview, Program and decoded recording demonstrate each supported picture/style/image change. Missing assets report a clear error. Unsupported controls do not appear actionable. Existing saved preferences migrate without erasing unrelated fields. |
| #764: routing/audio/camera | Audit `audioRole`, PTZ, participant `inputLevel`, virtual-camera size/FPS and shell roster/speaker/share commands when the Zoom engine is authoritative. Apply supported semantics or remove the misleading control/command. Keep deliberate test injection distinct from production authority. | Real PCM proves supported gain/routing changes. Unsupported PTZ is unavailable. Camera UI reflects the actual supported format; no quality downgrade is introduced. Production Zoom state cannot be overwritten by stale shell hints. |
| #763 | Carry the configured NDI group into sender creation and recreate only the affected sender when that configuration changes. Persist the choice and retire the old sender generation cleanly. | A real NDI receiver with matching/nonmatching discovery configuration verifies group behavior and rename/change recovery. Record unavailable runtime/receiver evidence as unverified. Group membership is a discovery feature, not an access-control guarantee. |
| #761 | Verify the existing director observation reaches `MagicSceneCoordinator` with valid slot bindings. Repair remaining mapping/execution defects within the existing coordinator and binding policies. Keep enabled state, recommendations, Preview changes and actual Takes distinct. | Real host-plus-guests, interview, solo and screen-share scenarios produce expected bindings and actions. Manual Take/hold, no eligible source, camera-off, departure/replacement, reconnect and disabling automation are safe. No duplicate guest bindings, unwanted host follow or silent “enabled” state that never acts. |

Split #764 by consumer seam into reviewable PRs referencing the same issue; do
not mix LUT pixels, audio routing and camera configuration in one patch. Keep
explicit unsupported behavior visible until a real consumer is qualified.
Automation must use the corrected observations and the existing director laws;
do not replace them with a second shell heuristic.

## Operator usability

| Issues | Implementation | Acceptance |
|---|---|---|
| #587 | Trace edit/binding/command timing and ensure Join reads the visible URL immediately. | Paste or edit and click Join once without focus loss: the intended sanitized meeting identifier is used. Include keyboard activation; never log credentials. |
| #592, #593 | Capture the unresolved lower-third incident with correlated metadata, serialized key, raster signature and output frame IDs. Repair the demonstrated cause. Add persistent per-source primary/secondary text overrides with explicit default/reset/empty behavior, using the existing source-name override. | Unchanged graphics remain stable; genuine edits update once without partial text or spurious transitions. Overrides follow source identity across slot reuse, guest replacement, scenes and restart. Preview, Program and decoded output agree. Do not guess at the lowercase-fragment cause. |
| #591 | Reproduce starter scenes on a fresh profile. Use distinct eligible inputs and intentional empty placeholders; preserve custom scenes. Reuse existing composition rather than introducing a new canvas editor. | One-person, two-person and screen-plus-presenter looks can be chosen, assigned, previewed and taken without silently repeating an input. Fresh and upgraded profiles both work. |
| #590 | Audit controls against operator tasks. Consolidate discovery, assignment, names, format/recording facts, role and dropout controls; remove the redundant Zoom health panel after its useful facts are retained. | A realistic Zoom/capture/media mix remains usable at normal window sizes. Ten-row refresh does not steal focus or selection. Assignments and saved controls persist. Existing #674 inspector facts remain the source of truth. |
| #594, #595 | Provide coherent Settings navigation containing Zoom, Health and support actions. Make OHG opt-in persistent and safe when its selected tab is disabled. | Keyboard navigation, support export and status refresh work. Fresh profiles hide OHG; enable/disable preserves configuration and returns to a usable tab. |
| #507 | Move developer detail into Health/Details and keep operator status concise. Reserve status space so error length cannot reposition transport controls. | Empty, long, failing and rapidly changing status strings never move or resize the controls. Full actionable errors remain reachable. Verify normal window sizes and supported display scaling. |

Reporting semantics precede navigation and transport polish; implemented settings
precede pruning their editors. Source text overrides and distinct scene bindings
must preserve stable source identity. #592 remains a diagnostic dependency only
for its defect fix, not an excuse to guess or block unrelated usability changes.

## Change and validation discipline

Use one focused issue/seam per PR, with at most three items in flight and one
code item active in the operator-focus sequence. No unconnected foundation PRs.
Each PR records the concrete before/after behavior, meaningful tests, current
limitations and whether its issue's full acceptance is met. Partial slices use
“Addresses,” not automatic issue closure.

Tests should exercise boundaries: contradictory lifecycle facts, stale run IDs,
async writer progress, healthy unchanged images, real command application and
manual intervention. Use existing native policy tests, MediaCore mapper/read-model
tests, WinUI coordinator tests and contract parity checks as appropriate. Simple
navigation/copy changes get a build and visual check, not implementation-mirroring
tests. Run focused checks during development and required hosted gates before merge.

At the end of each area, build a clean production Release package and verify
bundled-runtime startup. Exercise it with Zoom, a moving and a static screen/window
capture, media, recording/ISO and available output receivers. Preserve exact commit,
hardware, configuration, duration and logs. Use the local control API while the
operator uses the PC; preserve preferences and isolate test-run ownership.

Reporting qualification includes deliberately slow/unreachable outputs, retry,
stop/finalize and unplug/reconnect. Behavior qualification includes restart
persistence, actual pixels/PCM, NDI group discovery and automation/manual override.
Usability qualification includes fresh/existing profiles, normal window sizes,
keyboard operation and changing long status messages. The final combined bake
adds media Takes, guest transitions and source replacement to the stable workload.

Record PASS, FAIL or MISSING_EVIDENCE on each issue. Average FPS, advancing counters
and operator observation are useful but cannot substitute for required decoded
delivery evidence. An unresolved incident or unavailable physical device remains
explicit; it is not a pass. Existing #517/#781/#784 and HEVC acceptance stay tracked
separately and are not reopened as implementation work by this plan.

## Completion and release

The three areas are complete when their issue acceptance is satisfied on the
packaged app: trustworthy observed status, supported settings with demonstrated
effects, working automation, and usable persistent controls. Close issues only
after that evidence or an explicit owner disposition.

Cut betas at coherent area boundaries, not after every field correction. Keep
release notes scoped to demonstrated behavior, attach evidence and hashes, and
carry remaining acceptance forward. No new beta is required merely to publish
this plan, and no production stream or meeting is started as part of planning.
