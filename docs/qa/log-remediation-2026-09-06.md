# Log-driven reliability remediation — 2026-09-06

## Scope and validation status

The owner approved implementation after a read-only review of the September 6
logs, then explicitly resumed testing. Compilation, regression suites and live
checks have now run; candidate-specific results are recorded below. The candidate
is staged separately under `artifacts/log-remediation/win-unpacked`.

## Evidence and changes

- Five shutdown attempts failed with COM `0x8001010E` after a background
  continuation touched XAML. Shutdown now separates UI preparation from worker
  cleanup; activation also dispatches before accessing the window. OAuth must use
  the window's view model because its root content is a Grid.
- Deferred synchronization reported collection modification during startup.
  Production context and recurring spine payloads must capture UI-owned state on
  its owning dispatcher, with asynchronous handoff and stale-work guards.
  The supervisor's UI/poll sync slot also uses atomic acquisition. Shutdown
  cancels pending captures and suppresses queued UI updates; retired spine work
  is canceled rather than mislabeled as a request timeout.
- Unit tests deliberately generating preference and transport errors shared the
  production log destination. Test processes now use isolated session folders.
  Dated entries identify the process, build, session, and app/test role. Repeated
  exceptions retain a full first stack and report suppressed counts on recurrence.
- Native sync requests held the shared core lock for hundreds of milliseconds at
  startup. Command response JSON and preview event serialization now run outside
  that lock after capturing owning values. Command execution and response order
  remain unchanged; rendering is still required for command semantics.
- The old `emit->UIhandler` timing ended at a background subscriber, not the XAML
  dispatcher. New sampled timings distinguish receive age, parsing, dispatch
  queue time, and subscriber duration. Receive evidence survives dropped samples;
  sample number and child PID correlate the two entries. Local durations use a
  monotonic clock; cross-process receive age still depends on wall-clock stability.

## Verification checklist

1. Compile the managed shell/control projects and native core from this checkout.
2. Run focused lifecycle, synchronization, logging, transport, and native RPC tests.
3. Verify repeated normal closes release resources without UI-thread exceptions
   or forced exit; verify the bounded fallback separately for hung cleanup.
4. Exercise OAuth callback, concurrent roster/source edits during startup, and
   shutdown with an in-flight synchronization request.
5. Recheck source identity, Scene Builder preview, Take, lower thirds, and Magic
   Scene using actual native output evidence.
6. Repeat a comparable live soak, preserving build/session identifiers. Compare
   render worst-frame time, drops, command lock holds, response serialization,
   thumbnail receive age, parsing, and dispatch delay.
7. Build/publish a candidate only after reviewing the results. Do not label this
   revision live-verified based on tests from an earlier revision.

The existing logs do not isolate every stall. In particular, synchronous native
command/render work can still hold the core lock, and transport samples alone do
not establish GPU presentation or end-to-end meeting latency. No performance
improvement is claimed without a comparable run.

## Static review

Independent review checked native JSON ownership and command response ordering,
OAuth dispatcher access, queued control disposal, and shutdown watchdog lifetime.
Review findings were addressed: sampled receive logs remain available when frame
dispatch drops a sample, and the watchdog is held in a static field so collection
of a closed window cannot remove the fallback. Source review and whitespace
checks are not a substitute for compilation or runtime validation.

## Initial candidate results

- Managed suites: MediaCore 501, Control 54, WinUI 825 passed; native suite 629
  passed, with no failures or skips. Actual native outputs were built directly in
  `native/build-dev`, not its stale `Release` subdirectory.
- Candidate hashes and executable paths: `artifacts/log-remediation/qa/candidate-v1.json`.
- Real meeting: eight guests. Ninety native-confirmed operator cycles passed from
  12:21:39 to 12:25:18 Eastern. Native frame counter advanced 12,741 to 26,715.
  Take, lower-third on/off, one-shot Magic, and manual override assertions passed.
  A separate automatic-Take/manual-lower-third/manual-Take check also passed.
- Stress interval had no exceptions, bridge failures, or timeouts. Median render
  rate was 60 fps, minimum 56.9 fps, worst frame 121.2 ms, with 482 reported drops
  over 109 render summaries. There were 445 command-lock warnings, maximum 109 ms.
  This remains a performance limitation during rapid scene changes.
- Thumbnail transport: 112 samples, median 72.5 ms, p95 136 ms, maximum 185 ms;
  parsing max 29.7 ms, dispatch queue max 0.4 ms, subscriber max 0.8 ms. These
  measurements are not end-to-end meeting latency or a controlled comparison.
- Normal close at 12:26:06 released resources in approximately 207 ms. The shell,
  native core, and Zoom helper exited without forced exit or UI-thread exceptions.

Live testing exposed two additional problems before final-candidate validation:
the source picker changed its displayed choice without committing the route, and
one early sync reached the child before its handshake finished. The picker now
uses the event's added item and explicit template owner; startup waits for a
valid profile even when the child process is already running. Initial-candidate
results above must not be treated as validation of these subsequent changes.

## Final candidate results

- Final shell published to `artifacts/log-remediation/win-unpacked`; exact binary
  hashes are in `artifacts/log-remediation/qa-final/candidate.json`. This is a
  dirty checkout build, not a published release identified solely by Git HEAD.
- Regression coverage totals 2,010 passing cases: native 629, MediaCore 501,
  Control 54, and final WinUI 826. The complete MediaCore run preceded the
  handshake-test extension; all 11 focused handshake cases passed afterward.
  Final WinUI ran after the camera-readiness changes. No failures or skips.
- Startup readiness now also covers native camera connection, source enumeration,
  screen/window connection, and settings join. Passive startup sends require a
  validated profile. The final session connected the UVC camera at 1920x1080/30
  without the previously observed handshake exception.
- Final session `aa262b281fa444438553707ab27bf7ac` started at 12:38:22 Eastern.
  Ninety live native-confirmed cycles passed at 12:39:07–12:42:32, advancing
  native frames from 2,729 to 15,507. Separate automatic-Take and manual override
  checks passed. Results are under `artifacts/log-remediation/qa-final`.
- At 12:43:49, clicking the actual Studio Take button moved Panel to Program
  and Interview to Preview; native state confirmed both and frames continued.
  Automation and lower thirds were left off, with the application running.
- The preceding candidate's unchanged source-picker repair was verified live:
  selecting John updated routing and the rendered Scene Builder preview;
  clicking and dragging Source 4 preserved its routing. See the source-click
  report for the exact scope. No scene Save/Update was performed during this test.
- Three normal closes completed resource cleanup in approximately 207, 252,
  and 115 ms, without forced exit or UI-thread exceptions. Secondary-instance
  activation redirected to the primary window and the secondary process exited.
- Final bounded performance review found no logged exceptions or timeouts.
  Median render rate was 60 fps, minimum 56.3 fps, worst frame 94.3 ms, with
  272 reported drops. There were 256 command-lock warnings, maximum 89 ms.
  Thumbnail receive age was median 63 ms, p95 114 ms, maximum 183 ms.
  See `artifacts/log-remediation/qa-final/performance-review.md` for measurements
  and comparison caveats. Command/render lock contention remains unresolved.
- The desktop CoreVideo Pro shortcut now targets this tested candidate. Its
  previous shortcut is backed up beside the final QA results.

These checks do not establish a full OAuth authorization-code roundtrip, injected
hung-cleanup watchdog behavior, dedicated resize-handle behavior, a 30-minute
soak, or the full recording/streaming output chain. On meeting rejoin, the SDK
recording notice was acknowledged and capture was explicitly enabled for testing;
automatic capture after rejoin is not claimed as verified.

## Pre-merge recovery review

Independent review found a lock inversion between periodic bridge submission and
supervisor crash callbacks. Crash health/status callbacks now run outside the
supervisor gate, with child identity and Stop intent checked again before respawn.
Two deterministic regressions cover concurrent bridge/supervisor acquisition and
a subscriber stopping recovery. The full MediaCore suite passed 503 cases after
this repair. The earlier live candidate results do not include this final repair.
