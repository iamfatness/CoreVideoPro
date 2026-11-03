# Zoom SDK helper exit diagnostics: every exit leaves a cause — design

**Date:** 2026-10-23
**Status:** proposed design, awaiting owner review (decisions D1-D4 below are open)
**Incident:** [#856](https://github.com/iamfatness/CoreVideoPro/issues/856) (helper disappeared during a real meeting, 2026-10-10 and 2026-10-11)
**Related:** #790 (SDK share-end callback crash, a different confirmed mechanism), #725 (respawn roster barrier), #1033 (WER LocalDumps covers `corevideo-zoom-engine.exe` once installed), #433, `docs/reference/realtime-isolation-audit.md` row "SDK callbacks / recovery"
**Plan:** `docs/superpowers/plans/2026-10-23-zoom-helper-exit-diagnostics.md`

## Why

On 2026-10-10 at 09:06:24 EDT, installed `beta-2026-10-10-abb97fb` logged
`Zoom SDK helper connection lost. Rejoin the meeting to restore Zoom media.`
The WinUI shell and the native core stayed up. `corevideo-zoom-engine.exe` was
gone. There was no Application Error event and no WER dump, and the core's log
held no exit code, no last callback and no heartbeat age. The issue had to be
written as "a native exception, a termination or an orderly helper exit cannot
yet be distinguished".

The cause was eventually found by hand, outside the product. On 2026-10-11 the
same failure recurred (helper PID 55164, IPC loss logged 09:44:30). The owner
found a dump in a folder CoreVideo does not look in,
`%APPDATA%\ZoomSDK\logs\zoomcrash_094420_55164`. It was written by the Zoom
SDK's own crash handler, which this product turns on
(`init_param.enableGenerateDump = true`, `native/zoom-engine/engine/main.cpp:1454`).
cdb showed `bad allocation` thrown in `zoom_meeting_bridge.dll`, then
terminate, abort and the SDK fatal handler's deliberate null write. A Windows
Resource-Exhaustion-Detector event 2004 one minute earlier recorded system
commit at 99.82 %, and an ASUS service was found holding about 85 GiB of
shared sections. The October 10 dump (`zoomcrash_090616_45260`) was there too:
the crash came at 09:06:16, **eight seconds before** CoreVideo noticed anything.

Everything needed to name the cause existed on the machine within a minute of
the crash. The product captured none of it. The October 11 diagnosis took an
elevated handle census, a debugger and two owner interventions.

### Where exit information is lost today

| # | Site | What is lost |
|---|---|---|
| L1 | `native/src/modules/ZoomEngineProcess.cpp:247-253` `running()` | `GetExitCodeProcess` reads the exit code and discards it. Only the boolean comes back. |
| L2 | `native/src/modules/ZoomEngineRuntime.cpp:933-938` `readerLoop` | When `readEvent()` fails, the reader reports the generic "connection lost". It does not check whether the process is alive, read its exit code, or log `lastError()`. Lines 925-926 ("exited unexpectedly") carry no code either. |
| L3 | `native/src/modules/ZoomEngineRuntime.cpp:945-961` `reportEngineLossLocked` | Only the message string is logged. The client's `lastError_` and the Win32 error are never written anywhere. |
| L4 | `native/zoom-engine/shared/engine-ipc.h:322-333`, `ZoomEngineProcess.cpp:275-297` | EOF, a Win32 read error and a line over 64 KiB all return the same `false`. An unparseable line also returns `nullopt` from `readEvent()`. All four become "connection lost", and one malformed or oversized line retires the helper. |
| L5 | `native/src/modules/ZoomEngineProcess.cpp:209-225` `stop()` | The process handle is closed without reading the exit code. The core's own `TerminateProcess(..., 1)` uses the same code as `taskkill /F` and as the helper's own `return 1` (`main.cpp:1376`). A kill by the core cannot be told apart from a kill by someone else. |
| L6 | `native/zoom-engine/engine/main.cpp:1417-1425`, `native/src/modules/ZoomEngineClient.cpp:291-293` | The helper sends a ping every 2 s. The core writes it to the verbose log only and keeps no timestamp, so the last heartbeat age is unknown. On October 10 that age was about 8 s, the time Zoom's crash handler spent writing its dump. |
| L7 | `native/zoom-engine/engine/main.cpp:1365-1679` | The helper has no crash trap: no unhandled-exception filter, no `SIGABRT`, purecall or invalid-parameter handler, and no record of the last SDK callbacks. Zoom's handler takes the crash and the process ends without reaching WER, which is why #856 found "no WER event, no dump". |
| L8 | `native-shell/CoreVideoPro.MediaCore/Services/CrashDumpScanner.cs:31-34`, `SupportBundleArchiveBuilder.cs:46` | Both scan `%LOCALAPPDATA%\CrashDumps`. #1033's installer points WER LocalDumps at `%LOCALAPPDATA%\CoreVideoPro\crashes` (`scripts/alpha/Install-CrashDumps.ps1:25`). On an installed machine, even a WER dump of the helper would be missing from the crash offer and from `dumps.txt`. Zoom's `zoomcrash_*` folders are never listed. |

**In one sentence:** the core reads the helper's exit code and throws it away
(L1), reports every pipe failure as one generic "connection lost" without
checking the process (L2-L4), and the helper itself records nothing before it
dies (L6-L7), so the only evidence ends up in Zoom's own crash folder, which no
CoreVideo diagnostic reads (L8).

### Owner rules (2026-10-23)

- Honest reporting (`docs/reference/honest-reporting.md`). Every helper exit
  leaves a stated cause, or an explicit `unknown` that lists what was captured.
  The classifier never guesses.
- Nothing is uploaded publicly. Dumps and SDK logs can hold meeting credentials
  and media.
- Evidence location: in QA, on `D:` (the `COREVIDEO_CRASH_DIR` override); when
  installed, `%LOCALAPPDATA%\CoreVideoPro\crashes`.

## What this slice delivers

1. **Helper side.** A crash trap and a flight recorder. The flight recorder
   lives in shared memory owned by the core, so the evidence survives even an
   external `TerminateProcess` that runs no helper code.
2. **Core side.** An exit-evidence collector. It reads the exit code and the
   process times, decodes NTSTATUS values, reads the flight recorder, writes an
   out-of-process minidump when the trap asks for one, and inventories any dump
   from WER or from Zoom for that PID. It classifies the exit, writes one cause
   line and one JSON record, and publishes both in the snapshot.
3. **Surfacing.** A `zoom.helperExit` snapshot field (so `GET /snapshot` on the
   control API carries it), a `media-core.log` line and a `launch.log` line,
   and support-bundle inclusion. The shell's dump directories are corrected.
4. **Distinguished failures.** The IPC read result separates helper death,
   pipe I/O failure, protocol failure (oversized or unparseable line) and the
   SDK's own meeting or network disconnect, which already arrives as a
   `meeting_status` event and is unchanged.
5. **Fault injection.** The helper can be crashed on purpose so all of the
   above is proven. The trigger is double-gated: an environment variable arms
   it, and either a test IPC command or a timed environment trigger fires it.

It does **not** fix why the SDK ran out of memory. The October 11 trigger was
the machine's commit exhaustion, and the slice records commit headroom at the
moment of exit so the next occurrence names that directly. It does not add
automatic restart either; see "What must not be done".

## Design

### 1. Flight recorder: a core-owned shared section

Before spawning the helper, `ZoomEngineProcessClient::start()` creates one
pagefile-backed section of 64 KiB, committed up front, named
`Local\CoreVideoZoomHelperDiag_<instanceToken>`. It also creates two
auto-reset events, `..._TrapRequest` and `..._TrapAck`. The names are passed on
the command line (`--diag-section`, `--diag-trap-request`, `--diag-trap-ack`),
next to the existing `--ipc-token`. The core keeps the section mapped. When the
helper dies, however it dies, its last writes are still readable.

- **The size is fixed.** Windows sections cannot grow while mapped
  (CoreVideo's SHM resize deadlock, PR #189). All pages are committed at
  creation, so the helper never needs new commit to write evidence. This
  matters because the October 11 crash happened at 99.8 % system commit.
- **Names are scoped by token.** The instance token is scoped to a PID, so a
  test can never open or unlink a production section. That was the
  2026-09-20 vcam incident.
- **The layout is POD.** `native/zoom-engine/shared/helper-flight-recorder.h`
  is shared verbatim by the core, the real helper and the fake helper:
  - Header: `magic`, `version`, `helperPid`, `startFileTime`.
  - Heartbeat: `heartbeatSeq` and `lastHeartbeatFileTime`.
  - Lifecycle: `stage` (enum: `main_entered`, `ipc_ready`, `init_sdk`,
    `sdk_ready`, `auth_ok`, `join_sent`, `in_meeting`, `leaving`,
    `cleanup_sdk`, `exiting`) and `orderlyExit {reason, code, fileTime}`.
  - Crash evidence: `trap {kind, exceptionCode, address, moduleBase,
    moduleName[64], threadId, fileTime, exceptionPointersAddr, dumpRequested}`,
    `vehCandidate {code, address, threadId, fileTime}`, and
    `invalidParameterCount` with `lastInvalidParameterFileTime`.
  - Commit at trap: `processPrivateBytes`, `systemCommitBytes`,
    `systemCommitLimitBytes`.
  - Handler state: `faultInjected[16]` and `handlerChain {uefPrevModule[64],
    sigabrtPrevModule[64], reassertCount}`.
  - A ring of 256 entries `{fileTime, threadId, direction (in | out | sdk),
    tag[40]}` with an atomic sequence number.
- **Privacy.** A ring entry holds only the `cmd` and `stage` names, for example
  `in:join`, `out:meeting_status`, `sdk:onMeetingStatusChanged`, never a
  payload. No meeting ID, passcode, JWT, ZAK or display name can reach the
  section, so the section and the JSON built from it need no redaction.
- **Hook points.** Every outbound line goes through `EngineIpc::write`
  (`engine-writer.h:42`). It records its `cmd` (and `stage` for `debug`)
  before writing. `frame`, `audio` and `ping` are not ringed: `ping` advances
  the heartbeat fields, and the other two are high-rate media. Every inbound
  command is ringed at the top of the main loop. The SDK callbacks that set
  lifecycle (`onAuthenticationReturn`, `onMeetingStatusChanged`,
  `onUserJoin`/`onUserLeft`, `onRawDataStatusChanged`, `onSharingStatus`)
  ring a `sdk:` tag. A write is one relaxed `fetch_add` and a fixed `memcpy`,
  with no lock and no allocation, so it is safe from SDK threads and from a
  crash handler.
- **Degraded mode.** If the helper is launched without the arguments (by
  hand, or by an older core), it records into a private static buffer. The
  core then reports `flightRecorder: "unavailable: <reason>"` and never
  invents the fields.

### 2. The crash trap inside the helper

The trap is `native/zoom-engine/shared/engine-crash-trap.{h,cpp}`. It does not
depend on the SDK and is linked into both `corevideo-zoom-engine` and
`corevideo-zoom-engine-fake`, so CI can exercise it through the fake. It is
installed as the first statement of `main()`.

| Hook | Scope | What it does |
|---|---|---|
| `SetUnhandledExceptionFilter` | process | Stamps `trap` with the code, address, module and RVA (via `GetModuleHandleExW(FROM_ADDRESS)`), the thread, the time and the exception-pointers address. Records commit. Signals `TrapRequest` and waits on `TrapAck` for up to 20 s while the core writes the dump. Then it **chains** to the previous filter if there is one (Zoom's), or returns `EXCEPTION_CONTINUE_SEARCH` so WER LocalDumps still runs. |
| `AddVectoredExceptionHandler(0, ...)` (last) | process | Observes only fatal-class codes: `0xC0000005`, `0xC000001D`, `0xC0000096`, `0xC00000FD`, `0xC0000374`, `0xC0000409`. It writes `vehCandidate` and **always** returns `EXCEPTION_CONTINUE_SEARCH`. It never dumps, never blocks and never changes behaviour. Its only job is to timestamp a fault that another handler (Zoom's crashrpt) may take before our filter runs. |
| `signal(SIGABRT, ...)` | process (UCRT signal table) | Stamps `trap.kind = abort` and the thread, requests the dump, then calls the previous `SIGABRT` handler (the SDK's, which on October 11 performed the null write). This is the path `bad_alloc` takes: uncaught exception, then `terminate`, then `abort`, then `raise(SIGABRT)`. `std::set_terminate` is **not** used, because MSVC keeps terminate handlers per thread and ours would never be installed on SDK threads. |
| `_set_purecall_handler` | process | Stamps `trap.kind = purecall`, requests the dump, then `abort()`, which the `SIGABRT` path then records. |
| `_set_invalid_parameter_handler` | process | Stamps the count and the time. **D1** decides whether it then returns (absorb) or fail-fasts. |
| `atexit` and the end of `main` | CRT | Stamps `orderlyExit` with a reason: `quit_command`, `parent_pipe_closed`, `ipc_setup_failed` or `crt_exit`. |

- **Re-assertion.** The SDK installs its own handlers. The trap therefore
  re-installs the UEF and `SIGABRT` hooks at two points: immediately after
  `InitSDK` returns, and when the meeting reaches `in_meeting`. At each point
  it records which module owned the handler it replaced (`handlerChain`) and
  chains to it. This tells us, from evidence, whether Zoom's handler runs
  before or after ours, instead of us assuming either.
- **Handler discipline.** No heap use, no CRT I/O and no locks: only
  `InterlockedExchange`, `memcpy`, `SetEvent`, `WaitForSingleObject`,
  `GetModuleHandleExW` and `GetProcessMemoryInfo`, with
  `GetPerformanceInfo` resolved at install time. The handler does not write
  the dump itself. The core writes it from outside (§3.3), which still works
  when the helper's heap or stack is corrupt or commit is exhausted.
- **Distinct exit codes.** Every place the helper picks its own exit code gets
  a named value: `0` for orderly quit or parent pipe closed,
  `0x0000C701` for IPC setup failed, `0x0000C702` for an invalid parameter
  under D1 = fail. Code `1` is then never produced by CoreVideo code, and the
  core's own termination code becomes `0xC0DE0001` (§3.1).

### 3. The supervisor: exit evidence in the core

#### 3.1 Recording the facts at the moment of loss

`ZoomEngineProcessClient` gains `exitRecord()`. Its `ExitFacts` struct
contains:

- `exited`, `exitCode`, `exitedAtUtc` and `startedAtUtc` (from
  `GetProcessTimes`), `uptimeMs` and `pid`.
- `terminatedByCore` and `terminationContext`: one of `stop`, `rejoin`,
  `connect_timeout`, `shutdown` or `superseded`.
- `readFailure`, one of `eof`, `io_error(win32)`, `line_too_long(bytes)` or
  `none`; `parseFailures`; and `lastErrorText`.

`stop()` and `terminate()` set `terminatedByCore` and its context **before**
calling `TerminateProcess(handle, 0xC0DE0001)`, and read the exit code before
closing the handle. This closes L1 and L5.

At spawn the client duplicates the process handle (`DuplicateHandle`) for the
evidence collector. A Rejoin that closes the client's handle (`stopReader()`
leads to `terminate()`) then cannot race the collector's wait.

#### 3.2 The reader stops guessing

`ipc_read_line` gains a result-returning sibling, `ipc_read_line_ex`, that
returns `{ok | eof | io_error(code) | too_long(bytes)}`. `readEvent()`
returns either an event or a typed failure. `readerLoop`'s failure branch then
does three things:

1. It records the read failure in `ExitFacts`. If the failure was
   `too_long` or a parse error, **D2** decides whether the line is skipped
   and counted, or retires the helper as it does today.
2. It hands an exit job to the evidence collector and returns at once. The
   reader thread never waits, because `join()`'s `stopReader()` joins it.
3. It reports the loss through `reportEngineLossLocked`, now with a
   provisional cause. The existing warning text becomes
   `"Zoom SDK helper <classification summary>. Rejoin the meeting to restore
   Zoom media."`, for example "crashed (access violation)", "was terminated
   outside CoreVideo", "connection failed while the helper was still running"
   or "exited: cause unknown".

`ZoomEngineEventKind::Ping` handling sets `lastHeartbeatSteady_` in the
runtime. That closes L6.

#### 3.3 The evidence collector

The collector is one long-lived thread per `ZoomEngineRuntime`, created
lazily, with a bounded queue of at most 4 jobs. Further jobs are dropped and
counted, never blocked on. It holds **no** runtime `mutex_` and **no**
`coreMutex` while it waits or does I/O. It takes `mutex_` only for the final
publish, which is a pointer swap. Per helper process it does the following:

1. **Trap watch (for the process's whole life).**
   `WaitForMultipleObjects({TrapRequest, processDup})`. On `TrapRequest`, it
   reads `trap.exceptionPointersAddr` and `threadId` from the section and
   calls `MiniDumpWriteDump(processDup, pid, file, flags, &mei{ClientPointers
   = TRUE})` into `<crashDir>\zoom-helper-trap-<pid>-<utc>.dmp` (flags per
   **D4**). Then it sets `TrapAck`. If the dump fails, it records the Win32
   error in the record and still sets `TrapAck`.
2. **Exit wait.** After the process handle signals, or for up to 5 s after a
   read failure, it reads `GetExitCodeProcess` and `GetProcessTimes`. If the
   process is still alive after 5 s, it classifies `ipc_lost_alive`. It does
   **not** kill the helper; the existing Rejoin path does that.
3. **Snapshot the recorder.** It copies the 64 KiB section into memory. If
   the trap or the orderly-exit fields are mid-write (sequence numbers do not
   match), it waits 200 ms and copies once more.
4. **System commit.** `GetPerformanceInfo` records commit total and limit and
   physical available memory at observation time. The record says
   "observed at exit +N ms", never "at crash".
5. **Dump inventory, re-scanned at +0 s, +3 s, +10 s and +30 s.** Dumps can
   appear after the exit is observed: Zoom's handler took 8 s on October 10.
   The scan covers:
   - Our trap dump.
   - WER LocalDumps `corevideo-zoom-engine.exe.<pid>.dmp`, in `<crashDir>` and
     in the legacy `%LOCALAPPDATA%\CrashDumps`.
   - Zoom's `zoomcrash_*_<pid>` directory, located under
     `%APPDATA%\ZoomSDK\logs` per `docs/reference/zoom-capture-on-off.md:83`.
     Task 7 of the plan verifies the exact root on the dev box before
     hard-coding it, and keeps the candidate list in one constant.

   The scan matches on PID **and** on a time window
   `[startedAtUtc, exitedAtUtc + 60 s]`. It records paths and sizes only and
   never copies or moves a dump.
6. **Classify and write.** The pure classifier (§3.4) turns the evidence into
   `{classification, causeLine, evidenceComplete}`. After each scan the
   collector rewrites `<crashDir>\zoom-helper-exit-<utcStamp>-<pid>.json`
   (atomic write via temp file and rename) and republishes. After the +30 s
   scan the record is marked `evidenceComplete: true` and the job ends.

`crashDir` is `COREVIDEO_CRASH_DIR` if set (QA sets it to a folder on `D:`),
otherwise `%LOCALAPPDATA%\CoreVideoPro\crashes`, the same folder #1033's WER
keys use. The collector creates the directory if needed. If it cannot write
there, it keeps the record in memory, sets
`recordPath: "unwritten: <win32 error>"`, and the snapshot still carries it.

#### 3.4 The classifier (pure, table-driven)

`native/src/modules/ZoomHelperExitClassifier.{h,cpp}` takes `ExitEvidence`
(the facts, a recorder copy, the dump inventory and the commit sample) and
returns a classification. The rules are evaluated **in this order**, and the
first match wins:

| # | Evidence | Classification | Cause line (examples) |
|---|---|---|---|
| 1 | `terminatedByCore` | `terminated_by_core` | `terminated by CoreVideo during rejoin (code 0xC0DE0001)` |
| 2 | `trap.kind` set | `crashed` | `crashed: ACCESS_VIOLATION (0xC0000005) at sdk.dll+0xEFF00, thread 1234; dump <path>` / `crashed: abort() on thread 1234 (C++ terminate path); dump <path>` |
| 3 | Zoom `zoomcrash_*_<pid>` found | `crashed` | `crashed: Zoom SDK crash handler wrote <dir> (exit code 0x...); CoreVideo trap did not run` |
| 4 | WER dump for PID found | `crashed` | `crashed: <NTSTATUS name> (0x...); WER dump <path>` |
| 5 | `vehCandidate` set and no heartbeat or ring entry after it | `crashed` | `crashed: <NTSTATUS name> raised at <module+rva> <N> ms before exit; no dump captured` |
| 6 | Exit code is an NTSTATUS error (`0xC...`) | `crashed_unattributed` | `crashed: STACK_BUFFER_OVERRUN / fast-fail (0xC0000409); no CoreVideo, WER or Zoom dump found` |
| 7 | `orderlyExit` set | `exited_orderly` | `exited: parent pipe closed (code 0)` |
| 8 | Process alive 5 s after the read failure | `ipc_lost_alive` | `IPC <eof/io_error 109/too_long 70123 bytes/parse error> while the helper process was still running` |
| 9 | Exited, none of the above | `terminated_externally` | `terminated externally, code 1: no CoreVideo exit or crash path ran (consistent with TerminateProcess from outside, e.g. Task Manager or taskkill /F)` |
| 10 | Facts missing (no handle, no recorder) | `unknown` | `unknown: here is what was captured: <every field present, "absent" for each missing>` |

- **Why the exit code is checked last.** The exit code is only consulted after
  the stronger evidence. A crash taken by Zoom's handler may end with any
  code, perhaps `1`, which would otherwise read as an external kill. Rules 3-5
  exist so that case is reported as a crash.
- **NTSTATUS names.** A fixed table covers `0xC0000005`, `0xC000001D`,
  `0xC0000096`, `0xC00000FD`, `0xC0000374`, `0xC0000409`, `0xC0000017`,
  `0xC000013A`, `0x40010004`, `0x80000003`, `0xE06D7363` (C++ exception),
  `3` (CRT abort) and our own codes. `0xC0DE0001` is named "terminated by CoreVideo core" and is excluded from rule 6 even if the `terminatedByCore` flag was lost. An unlisted `0xC...` code prints as
  `NTSTATUS 0x...` with no invented name.
- **Every line carries the timing.** Each cause line appends
  `uptime=<s> lastHeartbeatAge=<s> lastEvents=[...last 8 ring tags...]
  commit=<used>/<limit> GiB`, with `absent` for any field that was not
  captured. A heartbeat age well above 2 s before death is how the
  October 10 signature (8 s with no heartbeat while Zoom's handler wrote its
  dump) becomes visible without a debugger.

### 4. Surfacing

| Where | What |
|---|---|
| Core snapshot | The core snapshot's `zoom` object (`CoreStateCapture.cpp:780` and `:1201`) and the raw-capture snapshot (`ZoomEngineRuntime.cpp:1326`) gain three fields. `helper` is present only while a helper is running: `{pid, generation, heartbeatAgeMs}`. `helperExit` is the latest record for the current or most recent generation, with `{generation, pid, classification, causeLine, exitCode, exitCodeName, startedAtUtc, exitedAtUtc, uptimeMs, lastHeartbeatAgeMs, lastEvents[], trap?, dumps{trap?, wer?, zoom?}, commit?, readFailure?, evidenceComplete, recordPath}`. `helperExitHistory[]` holds the last 8 records. The control API's `GET /snapshot` already returns this JSON; **no new route**. Fields that were not measured are absent, never zero. |
| `media-core.log` | One `[zoom-helper-exit] <causeLine> record=<path>` line per publish: provisional, then final. |
| `launch.log` | A pure `ZoomHelperExitObserver` (MediaCore) watches each applied snapshot. It deduplicates on `(pid, generation, evidenceComplete)` and writes `zoom-helper: exit pid=... gen=... class=... code=0x... uptime=... lastHeartbeatAge=... cause="..." record=...` once provisionally and once at final. |
| Operator | The existing Zoom error warning text carries the summary (§3.2). No new panel. |
| Support bundle | `SupportBundleArchiveBuilder` gains three things. (a) `helper-exits/` holds the 10 newest `zoom-helper-exit-*.json` from `crashDir`, at most 64 KiB each, passed through `SupportBundleLogRedactor` as a belt-and-braces measure. (b) `dumps.txt` lists `crashDir`, the legacy `%LOCALAPPDATA%\CrashDumps` and Zoom's `zoomcrash_*` directories, with names, sizes and dates only, as today. (c) `exit-context.txt` holds read-only System-log Resource-Exhaustion-Detector event 2004 entries within ±10 min of each included exit. Dumps are never packed. |
| Crash offer | `CrashDumpScanner` scans `crashDir` **and** the legacy folder (fixes L8). Whether helper dumps may join a consented crash-report zip is **D3**. |

### 5. Fault injection

Fault injection is double-gated and is never reachable from the shell, the
control API or saved preferences. This departs from
`docs/reference/fault-injection-seams.md`, whose rule is "no env var, no
command", for a stated reason: the helper is a separate executable that needs
the real SDK and real handlers to prove the property. A seam that only exists
inside `corevideo-native-tests` cannot crash it. The departure is documented
there.

- **Arm.** `COREVIDEO_ZOOM_ENGINE_FAULT_ARM=crash-this-helper` must be present
  in the helper's environment, which it inherits from the core and the core
  from the shell. Without it, both triggers are ignored, and they are logged
  as ignored.
- **Trigger A (tests).** The IPC command
  `{"cmd":"diag_fault","kind":"<kind>"}`.
- **Trigger B (installed, real meeting).**
  `COREVIDEO_ZOOM_ENGINE_FAULT=<kind>@in_meeting+<seconds>`.
- **Kinds.**
  - `av_write` (null write)
  - `throw_bad_alloc_thread` (an uncaught `std::bad_alloc` on a new thread,
    mirroring October 11)
  - `abort`, `purecall`, `invalid_parameter`, `fastfail`, `stack_overflow`
  - `exit_code_7` (`ExitProcess(7)`, a non-CoreVideo exit path)
  - `hang` (stop reading and pinging)
- **Visibility.** When armed, the helper writes `faultInjected` into the
  recorder and sends a `debug` stage `fault_injection_armed`. The cause line
  then **always** carries `(fault injected: <kind>)`, so a test crash can
  never be read as a field crash.
- **External kill.** No helper code is involved. The drill calls
  `TerminateProcess(pid, 1)` / `taskkill /F /PID`.

### 6. What must NOT be done

- **No automatic helper restart and no retry storm.** Helper loss keeps
  today's policy: `restartBeforeJoin_ = true`, and a fresh SDK process starts
  only on the operator's (or the core-recovery path's) explicit Rejoin. The
  evidence collector never spawns, signals or kills a helper. #597 showed
  what restart-on-failure does under pressure, and on October 11 an automatic
  respawn into 235 MiB of remaining commit would have crashed again.
- **No new respawn-barrier hazard (#725).** Every record carries the
  `processGeneration` and `rosterEpoch` of the helper that died. The shell
  attributes a record to that epoch only and never treats it as the current
  roster. Publishing a record does not bump `rosterRevision` and is not a
  roster fact.
- **Rejoin never waits on evidence.** The collector owns a duplicated handle
  and its own thread. `stopReader()` and `join()` never wait on it, and
  evidence work never holds `mutex_` or `coreMutex`. The realtime isolation
  contract holds: no SDK calls or I/O under state locks.
- **No in-process dump writing in the trap.** The helper may be out of
  memory, out of stack or heap-corrupt.
- **Do not swallow Zoom's crash handling.** We chain to it, so its dump (the
  one Zoom support can read) is still produced, and WER still runs when Zoom
  has no handler.
- **No payloads in the recorder** and no secrets in the JSON. No dump, SDK log
  or `zoomcrash_*` content leaves the machine, and nothing is copied into a
  bundle.
- **No invented cause.** If a field is absent, the line says `absent`. If no
  rule matches, the classification is `unknown` with the captured list.

## Owner decisions (open)

| # | Question | Recommendation |
|---|---|---|
| D1 | After recording an invalid-parameter event, should the helper continue (absorb), or fail-fast with code `0x0000C702`? | **Absorb, counted and surfaced as a warning.** ZComms v0.1.8 proved in the field that the SDK's post-init thread trips this handler and that absorbing keeps the session alive. Failing ends the show. |
| D2 | Should an oversized (>64 KiB) or unparseable IPC line retire the helper (today) or be skipped and counted? | **Skip and count, logged with a redacted 200-character prefix. Retire after 20 consecutive failures.** One bad roster line should not end a meeting. |
| D3 | May helper dumps (trap, WER, Zoom) be offered in the consented crash-report upload? | **No. Listing only.** They can hold JWT, ZAK and media. Matches "nothing uploaded publicly" and the #856 evidence handling. |
| D4 | Which trap dump flavour? | `MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules | MiniDumpWithHandleData` (a few MB; it recovered the October 11 exception message). `COREVIDEO_ZOOM_TRAP_DUMP=full` adds full memory in QA only. |

## Testing

- **Pure, CI on every platform.** Classifier rules 1-10 and their precedence,
  including "Zoom dump plus code 1 is crashed, not external". Also the
  NTSTATUS table, recorder layout (`static_assert` on size and offsets, ring
  wrap, torn-read retry) and `ipc_read_line_ex` over a pipe pair (EOF,
  oversized line, valid line).
- **Windows CI through the fake helper.**
  `corevideo-zoom-engine-fake` links the trap and the recorder. A native
  test spawns it through the real `ZoomEngineProcessClient` and the real
  collector, then:
  - `diag_fault av_write` gives `crashed`, a trap dump file and a JSON record.
  - `diag_fault abort` gives `crashed` with kind `abort`.
  - `TerminateProcess(1)` from the test gives `terminated_externally, code 1`,
    with the last ring tags present.
  - `quit` gives `exited_orderly`.
  - `stop()` gives `terminated_by_core`.
  - `hang` gives a growing `heartbeatAgeMs` and no classification while the
    process is alive.
- **Local Windows with the real SDK (`ZOOM_SDK_DIR`).** The same test is run
  against the real `corevideo-zoom-engine.exe` after `init`, so Zoom's
  handlers are installed. It shows whether Zoom's handler runs before or after
  ours (from `handlerChain`), that both dumps exist for `av_write` and
  `throw_bad_alloc_thread`, and that the classification is right for each.
  No meeting is needed: `InitSDK` installs the handlers without auth.
- **Installed acceptance (owner's machine, authorized test meeting).**
  Trigger B with `av_write@in_meeting+30`, then a separate run with an
  external `taskkill /F`. A support bundle is exported after each.

## Acceptance

1. A fault-injected crash in the real helper (`av_write`, and separately
   `throw_bad_alloc_thread`) during an authorized test meeting on an installed
   build produces all of the following:
   - A dump: the CoreVideo trap dump, plus Zoom's dump where its handler
     runs.
   - A `crashed` record whose cause line names the code, module+RVA and
     thread, and carries `(fault injected: ...)`.
   - `zoom.helperExit` in `GET /snapshot` with `evidenceComplete: true`
     within 35 s.
   - One provisional and one final `launch.log` line.
   - A support bundle containing `helper-exits/zoom-helper-exit-*.json` and a
     `dumps.txt` that lists the dump.
2. An external `taskkill /F` of the helper produces `terminated externally,
   code 1` with uptime, last heartbeat age and the last ring tags, and no
   claim of a crash.
3. The core's own Stop or Rejoin produces `terminated_by_core` with its
   context, never `terminated_externally`.
4. A forced protocol failure (an oversized line from the fake) produces the
   D2 behaviour, and its record or warning names `line_too_long`, never
   "connection lost".
5. After any of 1-4, Rejoin works on the first attempt. No helper is spawned
   without an explicit Rejoin, and Rejoin latency is unchanged (within 250 ms
   of the pre-change baseline on the same rig, three runs).
6. Every one of the ten classifier rules has a unit test, and the CI fake
   drill covers rules 1, 2, 7, 8 and 9.

Real-meeting acceptance of a **natural** recurrence remains open, as it does
in #856. This slice makes the next one self-explaining; it does not prove the
October triggers are gone.

## Risks, named

- **Zoom's handler may replace ours after our re-assertion points**, or hook
  `SetUnhandledExceptionFilter` itself. Mitigation: the VEH candidate stamp
  (rule 5) and the Zoom-dump inventory (rule 3) do not depend on our filter
  running. `handlerChain` reports what actually happened.
- **The out-of-process dump can fail under commit exhaustion**, because the
  core needs memory to write it. The record then says `trap dump failed
  (win32 <code>)`, and rules 2-3 still classify.
- **WerFault on CI runners** may hold the fake for a while after a
  `CONTINUE_SEARCH`. The fake calls `SetErrorMode(SEM_NOGPFAULTERRORBOX)` and
  the test's exit wait is bounded at 30 s. A runner timeout is a harness
  failure, not a pass.
- **An env-armed crash on a customer machine.** The arming value is a
  sentence, not a boolean. Every armed run is marked in its records, and
  neither variable is documented outside QA docs.
- **macOS.** `main-macos.mm` and the POSIX `stop()` (`waitpid` status thrown
  away at `ZoomEngineProcess.cpp:228-240`) are out of scope except that
  `ExitFacts` compiles there. A follow-up issue covers `WIFSIGNALED` and
  `WTERMSIG` decoding; it compiles in CI but has never run on a Mac.
