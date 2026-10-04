# CoreVideo Pro delivery repair completion plan

Owner approved, October 3, 2026. This plan explains how to finish and qualify the work in [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) and draft [PR #780](https://github.com/iamfatness/CoreVideoPro/pull/780), including the installation dependency tracked in [#781](https://github.com/iamfatness/CoreVideoPro/issues/781). The [render delivery specification](render-delivery-spec.md) remains the behavioral contract. [BACKLOG](../BACKLOG.md) remains the only ranked work queue; the sequence below describes engineering dependencies and proposes no automatic promotion or closure.

The intended result is continuous 1080p60 Program, virtual-camera delivery and multiview under the agreed show workload, without lower quality, extra configured buffering, or audio regression. The next proof needed is recovery during an actual camera publication race. Two favorable camera runs are insufficient to enable the experimental fix by default.

## Evidence supporting the plan

The original show logs confirmed monitor shedding and expensive source export work inside Program rendering. GPU capture and monitor isolation now have implementations and focused tests, but the full specification is not complete.

Windows was loading an obsolete development camera DLL. The obsolete registration has been removed. The installed candidate now starts after correcting machine registration and granting LocalService read/execute on its DLL; the installer must reproduce this reliably. This installation defect does not establish the cause of the original show stutter.

The current installed reader failed a measured minute with one repeated frame, four missing frames and a 78.825 ms arrival gap while Program recorded no underruns or deadline misses. A separately registered retry QA DLL subsequently passed two measured minutes, 3,601 frames each, with no identity losses and maximum intervals below 17.8 ms. Neither run exercised retry. These observations establish a downstream failure and diagnostic compatibility, but not a proven repair under contention.

Implementation baseline for this review is `37036a63`. The installed publisher is `0b9b5deb`; the QA DLL was built from the later reader plus the explicit diagnostic-target change. They must not be represented as one qualified release. Full raw evidence, hashes and cleanup records are in the owner's `preserved-local-evidence/render-repair-0b9b5deb/validation-report.md` outside this checkout.

## Work packages and dependencies

| Package | Concrete deliverable | Depends on | Exit evidence |
|---|---|---|---|
| A — Reproduce and locate camera loss | Qualified receiver and controlled contention experiment through Windows Frame Server | Existing identity trace and QA DLL | Baseline failure assigned to a specific boundary; deliberate receiver faults detected |
| B — Complete camera repair | Reader or publisher change at the proven failing boundary | A | Retry or other repair actually exercised; matched control comparison passes continuity and latency gates |
| C — Correct installation | Ownership-aware registration, DLL access, diagnostics and rollback in a separate #781 PR | Owner confirms dependency priority | Clean install, upgrade, removal and reboot tests load the intended DLL without manual edits |
| D — Finish render isolation | Independent ISO conversion, full consumer demand, prepared fallback images and complete resource lifecycle | Existing #517 implementation | Real-pixel tests plus failure injection show optional consumers cannot stall production |
| E — Complete production evidence | Bounded event tracing and versioned native/shell contracts | Start with A; finish as B and D stabilize | Every delivery boundary observable; trace loss prevents false acceptance; overhead within budget |
| F — Qualify and release | One reproducible package containing accepted changes | B, C, D and E | Controlled soak, installed rehearsal, A/V, resource and hardware gates pass |

A is the immediate engineering step. C and D can progress independently once their issue scope is approved; neither should be folded into an unrelated fix. Qualification begins only when the exact candidate and receiver are identified. This is not authorization to launch parallel agents.

## Demonstrate and repair camera contention

First qualify the receiver against a known-good, independently identifiable 1080p60 source. Verify that intentionally duplicated, dropped, reordered and torn identities fail the judge. Distinguish the camera's emitted sequence from received pixels so receiver scheduling loss cannot be misdiagnosed as a camera read failure.

Add an explicitly enabled diagnostic fault injector at the publication boundary, excluded from release builds. Sweep the consumer start phase and controlled short publication delays within one frame period. Exercise both odd-sequence contention and unchanged-publication reads. Include delays beyond the deadline as negative controls: these must produce accounted failures rather than a false pass. Record the injection schedule and actual duration; do not use arbitrary system load as the sole reproducer.

Compare retry off and on using otherwise identical reader code, producer, workload and buffer settings through the real OS camera. Pin and verify the loaded DLL before measurement. Repeat the same phase cases and use a fixed warmup of at most 30 seconds, preserving all failed trials. Count attempts, recoveries, held frames, payload-copy attempts, queue depth and content age. A successful case must actually encounter the intended race and recover it without losing a subsequent identity.

The experiment determines whether bounded waiting is sufficient. If the first divergence is publication or receiver scheduling, repair that boundary instead. If waiting cannot preserve both continuity and latency, review a bounded publication-handoff redesign before implementing it; preserve the existing camera pixel ABI or propose an explicit compatible migration. Do not extend waits indefinitely or increase Program buffering.

Exit requires an attributable failing control and repeatable recovery in the repaired case, including publisher restart, mapping replacement, camera reconnect, missing publisher and format mismatch. Invalid or missing mappings must terminate promptly. The existing limit of two payload-copy attempts per request and the nominal frame-period wait bound remain visible and tested. Scheduler overshoot must be measured, not described as a hard real-time guarantee.

## Make the camera install correctly

Use a separate PR for #781. Determine the supported installation location and required Windows service access before coding the migration. Confirm that only the intended reader DLL and any demonstrated dependencies need access; do not recursively broaden user-profile permissions.

The installer must resolve owned stale HKLM/HKCU registrations, register the selected installation for the actual service host, apply narrowly scoped read/execute permissions and verify activation. Detect another installation's ownership before replacing or removing a key. An older uninstaller must not unregister the newer camera. Preserve a usable rollback package, registration snapshot and settings.

Test clean user profiles, current and legacy upgrades, conflicting registrations, missing DLLs, denied access, in-use DLL replacement, older-version uninstall, service restart and reboot. Verify actual loaded module path and hash after activation. A fresh install with no development checkout must work without manual registry or ACL repairs. Surface an actionable installation error instead of reporting a live camera when activation failed. Keep cleanup limited to CoreVideo-owned resources.

## Complete the render architecture

Finish the remaining specification scope through focused types and existing core interfaces:

- Move ISO CPU readback/conversion to an independently scheduled, bounded branch. A slow CPU conversion must not block GPU capture publication; recording loss has explicit counters and retains its own continuity semantics.
- Complete typed consumer demand for Program, Preview, multiview, inspector, popout, editor and ISO. Hidden or destroyed optional consumers release demand. One consumer closing must not unsubscribe another active consumer.
- Prepare CPU fallback uploads and compatible source images off Program, including existing CPU-only adapters. Program consumes completed views; it performs no uploads solely for an optional monitor.
- Include monitor caches, exports, staging and retiring generations in the 256 MiB monitor budget; keep source ingress within 512 MiB. Report existing Program and encoder allocations separately. Admission refusal must be explicit and preserve quality.
- Carry source epoch, frame identity, capture time and color metadata through GPU and CPU alternatives. Complete resize, reconnect, adapter mismatch, device-loss and asynchronous retirement behavior, with at most one retiring generation per source.
- Verify the Program tile displays the actual buffered Program image and matching Take/layout identity. Measure monitor completion and shell presentation separately; submission alone is not displayed motion.

Test real pixels on an independent GPU device, color/range parity, 1080p and 1440p input, rapid Takes, demand changes and repeated device lifecycle transitions. Inject 25 ms monitor work, a stalled UI reader, exhausted monitor slots and slow ISO conversion. Optional branches may report degradation during those fault tests; Program and production audio must remain independent. Unsupported hardware paths remain unqualified rather than silently passing.

## Finish trustworthy diagnostics

Replace remaining synchronous camera hot-path file logging with fixed-size events in the specified preallocated 16 MiB trace buffer. Export in the background on explicit capture; aggregate at most once a second. Bound storage and count overwritten or lost trace events. Keep meeting secrets, names and media contents out of traces.

Complete the versioned `deliveryEvidence` native/C#/Swift contracts and fixtures. Carry exact frame/epoch identities across Program, publication, reader emission, monitor completion and receiver observation. Include interval and age distributions, capacity/refusal reasons and progress ages. Unknown older peers remain unknown. Reject malformed, partial or lossy evidence for a boundary-dependent pass.

Measure instrumentation on/off against the same workload: added p95 render-work cost must stay below 1%. Re-run with normal logging after diagnostic qualification. This package supplies the production-safe form of the focused evidence introduced for A.

## Qualify one final candidate

Build a Release package with a manifest recording commit, DLL and installer hashes, flags, driver/adapter, source formats, buffer setting, receiver version and workload. Reconstruct the original show configuration from saved evidence, including the 14-participant context and Display usage; explicitly record any unavailable source or workload substitution. Use a safe rehearsal or isolated baseline rig for comparisons with the original build. Do not reinstall the obsolete reader as part of the normal candidate setup.

Run short failure/recovery tests first, then a 30-minute controlled soak and a 90-minute installed rehearsal with Takes, participant churn, capture changes, inspector/popout visibility and ISO transitions. Run a separate combined recording, streaming and virtual-camera regression drill. Qualify one lower-tier machine before making broad hardware claims. Hardware or real-meeting dependencies that are unavailable remain `MISSING_EVIDENCE`.

| Gate | Requirement |
|---|---|
| Program | Zero new skipped slots, underruns, deadline misses or output sequence gaps after warmup; p99 CPU render work ≤ 8 ms and p99.9 ≤ 12.5 ms |
| Camera | Every expected synthetic identity delivered exactly once at verified 60/1; no tears or reordering; no unexplained interval over 33.4 ms |
| Multiview | Fresh output at configured 60 fps, no shedding under the reference workload, and correct Program-tile identity; distinguish display/vsync mismatch |
| Latency | No extra configured buffering; median content-latency regression ≤ 5 ms and p95 regression ≤ one 60 fps frame versus the matched baseline |
| Audio | No lost audio samples; absolute flash/beep skew ≤ 50 ms; first-to-last five-minute median skew drift ≤ 5 ms |
| Resources | All queues and residency within declared bounds; no sustained growth in memory, handles, threads or retiring generations |
| Evidence | Qualified receiver, actual loaded-module proof, complete trace, measured uncertainty and retained failed runs; no pass from averages alone |

The zero-deadline-miss gate follows the stricter repository acceptance requirement. Synthetic marker holds from a genuinely lower-rate input are distinguished from lost composed Program identities. An inconclusive measurement fails qualification until resolved. Full qualification takes at least two hours of timed runs, plus setup, baseline comparisons, combined-output tests and hardware testing; implementation duration depends on A's result.

## Review decisions and completion

For this review, confirm the full scope and acceptance gates above, and whether #781 should be ranked as a prerequisite to final #517 qualification. Proposed execution after review is A, then the proven camera repair, installation and render completion, followed by integrated qualification. Create scoped child issues for remaining #517 implementation slices and update BACKLOG only after the owner's ranking decision. Keep one issue per PR and preserve #780 as draft until its own scope is reviewable and validated.

Enable default flags only for the qualified supported workload after all gates pass. Present the final evidence, remaining limitations and rollback procedure for operator acceptance before release. On a delivery, audio, compatibility or latency regression, restore the last usable package and settings; do not restore the obsolete development registration or hide the failure by lowering quality. Close issues only after their required changes are merged and their acceptance conditions are met.

The owner approved executing this plan, including the #781 installation prerequisite. Execution and issue ranking are recorded in BACKLOG; flags and release acceptance remain governed by the qualification gates above.


