# CoreVideo Pro production architecture — completion plan

Status: proposed, 2026-09-09
Strategy this executes: `production-realtime-execution-plan.md`
Controlling architecture: `production-realtime-architecture.md`

## What this document is

The strategy document is good and does not need rewriting. What it lacks is an execution
plan sized against what the code actually does rather than what the execution record says
it does. This document supplies that. It was written after a three-lane audit of the live
tree at `13545fb`, with file and line citations throughout.

It contains four things: a correction to the record, a set of defects that can ship this
week independently of the migration, a lane-based execution order with the strategy's own
dependency errors fixed, and the decisions that need an owner.

---

## 1. Correcting the record

The execution record describes Waves 0 and 1 as landed and Wave 2 as partly landed. The
code does not support that reading. The record's recurring words are "gated",
"pre-cutover" and "disabled at startup", each of which implies a flag that could be
flipped. For most of Wave 1 there is no flag, because there is no construction site.

**Wave 1 is an unregistered island.** `native/src/core/MediaCore.cpp` (7400 lines) and
`native/src/rpc/JsonRpcServer.cpp` contain zero references to `AtomicTake*`,
`ShowStateOwner`, `SourceRegistry`, `AuthorityShadow`, `ShowPlanGenerator`,
`ShowPreparationTransaction` or `SceneVersionStore`. `MediaCore.h:3-19` includes no Wave 1
header. `AuthorityShadow` is constructed only by `native/tests/AuthorityShadowTest.cpp:9`.
The one exception is `ShadowExactSourceFrames` inside `ZoomEngineRuntime`, gated by
`COREVIDEO_EXACT_SOURCE_SHADOW` (`ZoomEngineRuntime.cpp:123`, default off), whose output
has no reader outside the test binary.

Three specifics worth stating plainly, because each is currently easy to misread:

- **The shadow comparison does not exist.** `AuthorityShadow.h:56-57` says so in a comment,
  and the `compared`/`matched` counters are never incremented anywhere in the `.cpp`. PR 07's
  stated substance is the comparison; none of it is written.
- **No golden scenario is executed by any adapter.** `test/data/wave1-authority.json` holds
  35 scenarios. Grepping `wave1-authority` returns three files: the doc, the test, and the
  validator. No C++ or C# code runs one. PR 07's exit gate is "all golden scenarios match";
  it currently stands at zero.
- **Take is not a native operation.** It is a client-side scene-id swap performed at
  `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportCoordinator.cs:151-152`,
  before anything is sent, rolled back on failure. macOS says the same in a comment at
  `mac-shell/Sources/CoreVideoProShell/AppModel.swift:1648`. There is no expected revision,
  no operation ID and no rendered observation on the live path. The `atomicTake` capability
  is not in `Protocol.h`, so a client cannot discover the feature even to be told it is off.

**Wave 2's new classes are parallel designs, not foundations under the incumbent.**

- `ProgramRenderWorker::Work` consumes `core::ShowPlans`. `MediaCore` produces
  `modules::CompositorRenderPlan` and has never heard of the former. Integrating PR 14
  therefore requires either landing PRs 06-08's cutover first, or writing an adapter that
  fabricates `ShowPlans` from legacy scene state, which is a second policy path and breaks
  the strategy's own rule 8.
- `ProgramPacketPlayout` re-implements scheduling that `native/src/modules/D3DProgramBuffer.h`
  already does, correctly, in production today at depth 3. That existing class already has a
  non-moving anchor, no padding or repair, real GPU fence evidence, and a three-owner
  keyed-mutex discipline. What it genuinely lacks is asynchronous generation-safe teardown
  (its destructor joins on the render thread inside `coreMutex`) and clock-epoch fencing.
- `coreMutex` is a stack local of `JsonRpcServer::run` (`JsonRpcServer.cpp:473`) passed by
  reference into `MediaCore`. Every change that claims to remove a lock crossing has to
  reckon with a lock that is not owned by the thing it protects.

**Wave 3 has no code**, which the record correctly does not claim otherwise.

The honest summary is that roughly 2,400 lines of well-factored, well-tested, fail-closed
substrate exist and are the right foundation, and that this is approximately the first third
of the work by effort and specifically the third that does not touch the legacy system.

---

## 2. Ship this week, independent of the migration

These are real defects in the shipping product. None of them depends on any wave. Doing
them first buys user-visible value while the long work starts, and three of them de-risk
later PRs by proving a property before a process boundary is introduced.

| # | Defect | Fix | Size | Why now |
|---|---|---|---|---|
| S1 | **Every ISO stem loses about 17% of its motion.** `submitIsoVideo` is called from the audio worker at `MediaCore.cpp:6563-6568`, so ISO video is muxed on the 20 ms audio grid. A 60 fps source cannot exceed 50 fps in its file. | Move the call into `renderVideoOutputTick` beside the Program submit at `:6893`, sourcing `isoSources` from the same `coreMutex` gather. | S | Live product defect. Also proves the ISO submit is cadence-independent before PR 24 moves it across a process boundary. |
| S2 | **Audio loss is unmeasurable.** The FIFO shed site `AudioDsp.h:773-776` increments `shedSamples`, which is never published. `scripts/qa/production-qualification.mjs:14` requires an `audioLostSamples` counter that no producer emits. | Publish it in the audio worker node of `sessionState`. | XS | Closes PR 18's exit gate observability and unblocks any G2 evidence run. |
| S3 | **The G2 judge is built and starved.** `scripts/qa/production-qualification.mjs` and `runtime-snapshot-qualification.mjs` are genuinely fail-closed and refuse submitted counters. Nothing samples `sessionState` on a schedule and writes the envelope they consume. | A collector that samples and writes `{samples[], expectedWorkers, recordingExpected, policy}`. | S | Cheapest possible unlock of an aggregate-counter G2 story. |
| S4 | **A core crash orphans a live stream.** The output FFmpeg children have no job object; `grep JOB_OBJECT` returns zero hits in `RtmpOutputSenderAdapter.cpp`. `SrtIngestCaptureAdapter.cpp` already shows the pattern. | Add `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` to the output spawn. | S | An orphaned encoder holds a network endpoint after the app dies. |
| S5 | **The drill blames a coupling that no longer exists.** `scripts/mac-show-drill.py:372-377` hard-codes "encoder->submit rides the ~50Hz audio worker". For Program video that was fixed; the live submit is on the signalled video tick. | Print observed render fps, video-tick rate and dropped-frame counts so the message names the actual stage. | S | The drill exists to prevent true-but-misleading evidence. It is currently producing some. |

Already landed today, in the same spirit: a use-after-close race on the SRT ingest
descriptors, a destructor-versus-shutdown lifetime bug in the new render worker, and two
CI harness defects that were failing jobs this branch had turned red.

**One caveat on S5.** The macOS drill reads 24.7 fps, which is not a 50 Hz alias; aliasing
would read near 50, and Windows history recorded exactly that. The likelier cause is that
the macos-14 runner cannot composite 60 at all, which the CI workflow itself half-admits by
marking the loaded step `continue-on-error`. Confirm by printing the stage numbers before
planning against it.

---

## 3. Execution order: three lanes, not four waves

The strategy reads as sequential waves, but its own dependency graph does not require that.
Wave 3 explicitly starts on synthetic media packets once Wave 0 contracts exist, and Wave 0
is done. All three lanes are open now. Running them sequentially would cost months for
nothing.

### Lane A — control plane, to G1

The G1 gate asks that one native dataset answers who is present, which source generation is
usable, who is in the show, who gets an ISO, what Preview contains, and what Program
actually rendered. Zero of those six clauses hold today.

| Order | Work | Size | Note |
|---|---|---|---|
| A1 | Register the atomic Take RPC: a `JsonRpcServer` branch, the `atomicTake` capability across `Protocol.h` and its two protocol mirrors, and the `Apply` callback wired to a real owner | M | The cheapest genuine gap in Wave 1. Codec, coordinator and adapter are already done and tested. |
| A2 | Shadow runtime hooks: the startup flag that does not yet exist, the capture seams, worker lifecycle, side-effect spy tests | M | Seams are already located to line numbers in the design doc. |
| A3 | **Implement the comparison and its diagnostics** | L | Does not exist. Needs legacy plan observation bound to an exact basis pair, divergence classification, bounded redacted records, and an `authorityShadow` snapshot node. This is PR 07's actual substance. |
| A4 | Golden-scenario adapters executing all 35 scenarios against the real C++ owners | L | Includes a contract extension: `PlannedAudioEligibility` (`ShowPlanGenerator.h:80`) can only report audio as unknown, and the goldens require independent audio availability and freshness. |
| A5 | Real source ingest into `SourceRegistry` for capture, UVC, WGC, browser, SRT and media, plus durable person-identity evidence | L | The live observation covers Zoom camera and share only. No non-Zoom source class has an identity model. |
| A6 | Migrate MediaCore control state to `ShowStateOwner` and delete the second store | L, probably two | `applyCommandMutation` is a 24-branch chain over 7400 lines of member state. This is the real G1 blocker and it cannot be flag-flipped. Split per domain. |
| A7 | Production `IShowResourcePreparer` implementations with owning leases | L | All five implementations today are test doubles named `Ready`. |
| A8 | WinUI cutover: transport shim, capability negotiation, replace the local swap, consume `sourceAuthority`, operation-ID reconciliation | L | The shell must receive native Preview revision, plan stamp and preparation certificate, none of which is published today. |
| A9 | macOS adoption, then OHG and Companion | M-L, then S-M each | All three start from zero contract. |

### Lane B — real-time substrate, to G2

| Order | Work | Size | Note |
|---|---|---|---|
| B1 | Fault-injection seams: blocked monitor render, blocked Present, forced device removal | M | Nothing exists. `grep injectFault` returns nothing anywhere. This must precede PRs 16 and 17, because neither can prove its own exit gate without it. |
| B2 | Per-slot rendered and delivered records exported from the program buffer | M | It already knows every slot's outcome and discards it into counters. |
| B3 | **Cross-device source-texture sharing** | L | Unowned by any PR in the strategy. A monitor compositor on a second device must either re-upload every source every tick, which is roughly 1.5 GB/s at the admitted load and defeats the GPU path, or share a source ring that does not exist. This is the PR 20 transport problem arriving four PRs early. |
| B4 | PR 16, independent monitor compositor for Preview and Multiview | L+ | Depends on B3. Good news: the Multiview Program cell already consumes the delivered packet rather than reconstructing intent (`MediaCore.cpp:3243-3253`). |
| B5 | PR 14, `ProgramRenderWorker` takes ownership of the Program context | L | **Cannot pass its exit gate before B4.** You cannot remove GPU work from under `coreMutex` while Preview and Multiview share the Program context. Also blocked on either Lane A's cutover or an accepted adapter, per the `ShowPlans` type mismatch. |
| B6 | PR 15 rescoped: give the existing program buffer asynchronous generation-safe teardown and clock-epoch fencing | M | Not a replacement. The existing class is correct on the parts that matter; its destructor joining on the render thread inside `coreMutex` is the real defect. |
| B7 | PR 17, presentation service: Present off the UI thread, real device-loss recovery, frame-latency admission, teardown generations | L | About 60% of the named deliverable already ships and is battle-tested: shared device, ingest broker, swap chains, teardown safety. The remaining 40% is the two hardest items, each a PR's worth. Today a dead device leaves every host on CPU fallback until restart, because `s_sharedDevice` stays non-null forever. |
| B8 | PR 18, audio worker without control-lock crossings | M mechanism, L evidence | The worker is already the right shape: dedicated thread, MMCSS, bounded catch-up, loud reanchor. The work is evicting four encoder and sender submits that need a Wave 3 destination first. |
| B9 | Presented and display-completion export from the shell | L | The genuinely hard one, and the same problem as B7. |

### Lane C — outputs and recording, to G3

| Order | Work | Size | Note |
|---|---|---|---|
| C1 | PR 22, truthful destination lifecycle, moved ahead of PR 21 | S-M | Smaller than stated. The lifecycle contract exists with the full state set, and the async encoder sink already drives most of it. Remaining: rename and add two states, require fresh progress, stop Stop from claiming stopped before the barrier drains, extend to senders. Landing this first gives PR 21 a real health surface to assert against. |
| C2 | PR 25's platform capacity probes, started early and in parallel | L | Today's capacity is a hard-coded `8/1/true/true` literal at `MediaFoundationEncoderAdapter.cpp:1505-1510`. Replacing it with a probed struct is valuable on day one and converts a silent hardware-to-software spill into an honest one. The probes are the long pole and have no dependency on 23 or 24. |
| C3 | PR 19, output supervisor | M | Four existing supervisors already agree on a 5/10/20/40/60 second ladder with give-up at five. Copy it. Health must be fresh evidence, never a launch: a frame, thirty seconds alive, a handshake, an engine-reported status. Add the job object here, not in PR 28. |
| C4 | **PR 20a, the transport ADR spike, as a separate deliverable with no shipped code** | Un-sized, budget it | The gate already mandates a measured prototype of two designs under Program plus seven ISOs. The repo has zero measurements in this direction. The keyed-mutex export is single-consumer by construction, so the GPU option means eight dedicated textures and eight keyed-mutex owners, not one. Establishing that alone may take a week. |
| C5 | PR 20b, the chosen transport | L | |
| C6 | PR 21, recorder host | M-L | The COM and Media Foundation re-owning is M. But moving the sink out of process turns the recording session request, ISO frame and output session surfaces into a wire protocol. That is a contract PR hiding inside an M. |
| C7 | PRs 23, 24, 26, 27 | L, L, M, L | PR 26 is genuinely M because `scripts/validate-recording-finalization.mjs` is already most of the validation harness, and it is the strongest artifact-truth asset in the repo. |
| C8 | PR 28, streaming and vcam under supervision | M for RTMP and SRT, S for vcam, plus a fourth adapter | **NDI is a fourth adapter and the riskiest.** It is the only in-process third-party DLL doing blocking paced sends. RTMP and SRT are already out-of-process, so for them this is re-parenting an existing child, not new isolation. |

### Integration, to G4

Lane-crossing work only after G1, G2 and G3: connect the supervisor to the authoritative
delivered packet, startup-selected execution path with capability negotiation, remove legacy
ownership after a parity and usage audit, then exact-package qualification.

---

## 4. Corrections to the strategy's sequencing

Six dependency claims in the strategy do not survive contact with the code. Each is
reflected in the lane order above.

1. **PR 16 must precede PR 14's exit gate**, not follow it. The strategy lists 16 as
   depending on 13 and 15.
2. **Cross-device source-texture sharing has no owning PR.** It is a prerequisite of 16.
3. **PR 18's dependencies omit an output destination.** It cannot stop crossing the control
   lock until the four encoder and sender submits have somewhere else to go.
4. **PR 22 should precede PR 21.**
5. **PR 20 should split** into a measured ADR spike and the transport itself, so a
   measurement exercise is never reported as implementation progress.
6. **PR 15 should be rescoped** from replacement to hardening of the existing buffer.

---

## 5. What this unlocks for OHG

Three of the nine capability gaps in the hardware-parity assessment are architectural, and
two of those three are downstream of this work rather than of anything in the OHG codebase.

- **Non-Zoom sources being evicted from OHG slots.** The root cause is that the show engine
  models panelists rather than inputs. `SourceRegistry::Kind` is already
  `{ParticipantVideo, ParticipantShare, Device, Media, Browser}` with person identity held
  separately from source identity. Once Show Inputs project from the registry, a slot holds
  a source of any kind. This is Lane A, items A5 and A6.
- **Zoom screen share not being routable.** Share is a first-class kind in the same registry.
  Today the share route mode nulls the participant identity and falls through to positional
  binding.
- **The empty-box bug is the same bug.** `MediaCore.cpp:4958` passes `frameIdentity` as a
  hard-coded `nullptr`, so every exact-source route resolves as missing, and the positional
  fallback at `RouteSourcePolicy.h:115-121` hands a box whatever frame sits at its index.
  Integrating source leases (Lane B) closes it properly rather than by convention.
- **Program-versus-preview tally with real evidence.** Delivered packet identity is what
  distinguishes what was requested from what actually rendered, which is exactly the
  distinction tally needs and currently guesses at.
- **A video aux bus for the gallery wall.** Per-destination ownership in Lane C is what makes
  a second video output tractable instead of a rewrite.
- **Somewhere for transitions to live.** The atomic Take contract carries a transition
  definition and the render worker owns animation state. Today Take is a scene-id swap with
  no A/B render, so there is nowhere to put a dissolve.

It does nothing for three OHG gaps that remain small independent fixes: gallery cell
ordering, black and bars and fade-to-black, and the dropped nameplate location.

---

## 6. Decisions needed

1. **Scope and appetite.** Twenty PRs remain, most sized large, plus a transport spike and
   a hardware qualification matrix. Even with three lanes genuinely parallel this is a
   quarter or more of sustained engineering. The alternative is to take Lane C and the
   ship-now list only, which fixes recording and output truth without touching the control
   plane. That is a legitimate smaller programme and it should be an explicit choice rather
   than a drift.
2. **The transport spike.** It is the schedule risk in Lane C and it cannot be sized from
   existing evidence. Budget it as its own deliverable or it will absorb PR 20 and blow out.
3. **A shadow window.** Lane A's comparison is only worth building if it runs against a real
   show. Is there a window to run shadow mode alongside a live OHG broadcast, and who watches
   the divergence report.
4. **G4's matrix.** The gate names two specific machines and five scenarios. Confirm both
   machines are available, since release evidence must come from the packaged binary.

---

## 7. On sizing honestly

The strategy sizes PRs 05, 07, 08, 10 and 11 at M. The audit puts 07, 08, 10 and 05 at L,
and 06 at two L's. Wave 2's 16 and 17 are both larger than L as listed, for reasons the
strategy does not name. Wave 3's 22 is smaller than stated and 25 is probably the largest
single PR in the programme.

The most useful thing this plan can do is stop the record from reading as further along than
it is. The substrate that exists is genuinely good work with correct fail-closed semantics
and honest comments; several classes say plainly in their own headers that they are not
registered. The problem is not the code. It is that a reader of the execution record cannot
tell the difference between a capability that is built and disabled and one that has no
construction site, and those two states are months apart.
