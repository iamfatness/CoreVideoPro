# Zoom Helper Exit Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every exit of `corevideo-zoom-engine.exe` leaves a stated cause, or an explicit `unknown` that lists what was captured. That evidence goes into the snapshot, `media-core.log`, `launch.log`, a JSON record and the support bundle, with no new restart behaviour.

**Architecture:**
- **Flight recorder.** The core creates a fixed-size, pre-committed shared section per helper. The helper keeps it current with heartbeat, lifecycle stage and a ring of cmd/stage names.
- **Crash trap.** It lives in the helper and stamps the section: unhandled-exception filter, a last-position VEH, `SIGABRT`, purecall and invalid-parameter. It then asks the core for an out-of-process minidump and chains to Zoom's handler.
- **Evidence collector.** A core-side thread reads the exit code, process times, the section, commit, and WER/Zoom dumps for that PID. A pure classifier turns the evidence into a cause line.
- **Shell.** It reads the snapshot field, writes `launch.log`, and packs the records and listings into the support bundle.

**Tech Stack:**
- C++20 core (`native/`) and the in-house gtest shim (`native/tests/gtest/gtest.h`).
- Win32 (`DbgHelp` `MiniDumpWriteDump`, `GetProcessTimes`, `GetPerformanceInfo`).
- .NET 9 shell (`CoreVideoPro.MediaCore`, `CoreVideoPro.WinUI`) with xUnit as already used in `CoreVideoPro.MediaCore.Tests`.
- PowerShell for the QA drill.

**Spec:** `docs/superpowers/specs/2026-10-23-zoom-helper-exit-diagnostics-design.md` (rules L1-L8, classifier rules 1-10, decisions D1-D4).

**Recommended executor:**

| Task | Model | Effort | Why |
|---|---|---|---|
| 0. Preflight and owner decisions | sonnet | low | Re-grep line refs, record D1-D4 rulings, Release baseline build. |
| 1. Pure classifier + NTSTATUS table | sonnet | medium | Table-driven pure code; the precedence tests carry the risk and are fully specified. |
| 2. Flight recorder layout | sonnet | medium | POD layout, `static_assert`s, ring and torn-read logic; mechanical but must be exact. |
| 3. Typed IPC read failures (D2) | opus | high | Changes the reader's fatal/non-fatal semantics on the live path; easy to reintroduce "one bad line kills the show" or a reader that never exits. |
| 4. Process client exit facts + diag section | opus | high | Handle lifetime, `DuplicateHandle`, terminate ordering and the Rejoin race; mistakes look fine in unit tests. |
| 5. Helper crash trap + recorder hooks | fable | high | Async-signal/handler-safe code, handler chaining with Zoom's crashrpt, re-assertion points. A wrong handler either hides the crash or causes one. |
| 6. Fault injection (double-gated) | sonnet | high | Small code, but the arming gate is a safety property. |
| 7. Evidence collector + runtime integration + snapshot | opus | high | Lock discipline (no `mutex_`/`coreMutex` while waiting), out-of-process dump, timed re-scans, the fake-helper drill test. |
| 8. Real-SDK fault test (local, Windows + `ZOOM_SDK_DIR`) | opus | high | Interprets how Zoom's handlers actually interact with ours; the result can force a design change. |
| 9. Shell surfacing + bundle + scanner fix (D3) | sonnet | medium | Follows existing `SupportBundleArchiveBuilder` patterns; pure observer is table-tested. |
| 10. QA drill script + installed acceptance | fable | high | Crashes the helper on the owner's installed app in a real test meeting; a false pass is expensive. |
| 11. Docs | sonnet | low | Docs record measured outcomes and need judgement about what was and was not proven. |
| Review (once, after Task 10) | opus | high | Per Review Focus. |

**Estimated AI-assisted effort:**

| Task | Hours | Note |
|---|---|---|
| 0 | 0.5 | |
| 1 | 2 | |
| 2 | 1.5 | |
| 3 | 2.5 | |
| 4 | 3 | |
| 5 | 4 | |
| 6 | 1.5 | |
| 7 | 5 | |
| 8 | 2.5 | |
| 9 | 3 | |
| 10 | 2.5 | owner present for the meeting runs |
| 11 | 1 | |
| Review and fixes | 2 | |
| **Total** | **about 31** | range 26-38 h |

Tasks 1, 2 and 9 can run in parallel. Tasks 3-4 must finish before 7. Tasks 5-6 must finish before 7's drill test. Task 8 needs the SDK machine.

## Global Constraints

- **Windows build required.**
  - Tasks 4-8 and 10 must be built and run on the local Windows dev box. Real-helper work (Task 8, Task 10) also needs `ZOOM_SDK_DIR` pointing at the staged x64 SDK.
  - CI's Windows job covers the pure tests and the fake-helper drill (Task 7). The real helper cannot run in CI.
  - Mac CI only compiles the cross-platform parts; nothing here is run on a Mac.
- **Release only.**
  - Build: `cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native corevideo-zoom-engine-fake corevideo-zoom-engine`.
  - Debug and Release write the same exe path. Check the size before believing anything (Release core is about 2.2 MB, Debug about 8 MB).
  - If a new test does not appear in the output, check which binary ran.
- **gtest shim.** `--gtest_filter` takes ONE positive wildcard: no `:` lists, no `-` negation. Run suites separately.
- **No automatic restart.**
  - Nothing in this plan may spawn, signal or kill a helper except the existing Stop/Rejoin/shutdown paths and the tests' own fixtures.
  - `restartBeforeJoin_` semantics are unchanged (spec §6).
- **Lock discipline.**
  - The evidence collector never holds `ZoomEngineRuntime::mutex_` or `coreMutex` while waiting, scanning, dumping or writing files. It takes `mutex_` only to publish.
  - `stopReader()` and `join()` must never wait on the collector.
- **Handler safety.** Code reachable from the trap uses no heap, no CRT stdio, no `std::mutex` and no `std::string`. It is allowed fixed buffers, `Interlocked*`, `memcpy`, `SetEvent`, `WaitForSingleObject`, `GetModuleHandleExW` and `GetProcessMemoryInfo`.
- **Privacy.**
  - The recorder stores only `cmd`/`stage` names, never a payload.
  - No dump, SDK log or `zoomcrash_*` content is copied into any bundle or uploaded.
  - The QA crash dir is set with `COREVIDEO_CRASH_DIR=D:\CoreVideo-QA\evidence\zoom-helper-exits` (create it; never commit its contents).
- **Owner's box.**
  - Never run native suites while the owner's installed app is streaming or has the virtual camera on.
  - If you swap the fake over the real helper for any manual check, back up each copy as `*.realbak` and restore it (CLAUDE.md).
  - Real-meeting runs use the authorized test meeting only, with the full pwd link.
- **Honesty.**
  - An absent measurement is absent in JSON, never `0`.
  - A cause line never names a cause the evidence does not show.
  - Every fault-injected record carries `(fault injected: <kind>)`.

## Review Focus

For the reviewer (give this to the review agent verbatim):

1. **Precedence is right.** Re-derive classifier rules 1-10 from the spec and confirm that a Zoom `zoomcrash_*_<pid>` dir combined with exit code `1` classifies as `crashed`, not `terminated_externally`. Also confirm that `0xC0DE0001` never reaches rule 6.
2. **The trap is handler-safe.** Read every function reachable from the UEF, VEH, `SIGABRT`, purecall and invalid-parameter handlers. Flag any allocation, lock, CRT stream or `std::string`. Confirm the VEH always returns `EXCEPTION_CONTINUE_SEARCH`, and that the UEF chains to the previous filter or returns `CONTINUE_SEARCH`.
3. **No restart was added.** `git grep -n "ensureStarted\|start(" native/src/modules/ZoomEngineRuntime.cpp` shows no new call sites. The collector never calls `start`, `stop`, `terminate` or `TerminateProcess`.
4. **Locks.** The collector's waits and I/O happen outside `mutex_` and `coreMutex`. `stopReader()` joins only the reader. The duplicated handle is closed exactly once.
5. **Gating.** Both fault triggers are inert without `COREVIDEO_ZOOM_ENGINE_FAULT_ARM=crash-this-helper`. A test proves the ignored case.
6. **Privacy.** Grep the recorder writes and JSON writer for any field that could carry a meeting ID, passcode, JWT, ZAK or name. Confirm the bundle copies only `zoom-helper-exit-*.json` files and listings.
7. **Evidence of watched failure.** Each new test was seen failing before its implementation. Paste the RED output in the task report.
8. **Real-SDK result recorded.** Task 8's `handlerChain` finding (whether Zoom's handler replaced ours, and which dumps appeared) is written into the spec's Outcome section with the run's SHA.

---

### Task 0: Preflight and owner decisions

**Files:**
- Modify: `docs/superpowers/specs/2026-10-23-zoom-helper-exit-diagnostics-design.md` (D1-D4 rulings only)

- [ ] **Step 1:** Re-grep the L1-L8 sites. If a line moved, update the spec table; the plan's tasks cite function names, not lines.

```bash
grep -n "GetExitCodeProcess\|TerminateProcess" native/src/modules/ZoomEngineProcess.cpp
grep -n "connection lost\|exited unexpectedly\|reportEngineLossLocked" native/src/modules/ZoomEngineRuntime.cpp
grep -n "enableGenerateDump\|\"ping\"" native/zoom-engine/engine/main.cpp
grep -n "CrashDumps" native-shell/CoreVideoPro.MediaCore/Services/*.cs
```

- [ ] **Step 2:** Get owner rulings on D1-D4 and write them into the spec's decision table with the date. **If D1 or D2 is ruled differently from the recommendation, Task 3 or Task 5 follows the ruling.**
- [ ] **Step 3:** Run a baseline Release build and full native suite. Record the pass count and the core exe size in the task report.

---

### Task 1: The pure classifier and NTSTATUS table

**Files:**
- Create: `native/src/modules/ZoomHelperExitClassifier.h`, `native/src/modules/ZoomHelperExitClassifier.cpp`
- Create: `native/tests/ZoomHelperExitClassifierTest.cpp`
- Modify: `native/CMakeLists.txt` (add the `.cpp` to `corevideo_native` sources and the test to `corevideo-native-tests`, next to `tests/ZoomEngineStateTest.cpp`)

**Interfaces:**
- Produces:

```cpp
namespace corevideo::modules {
enum class HelperExitClass { TerminatedByCore, Crashed, CrashedUnattributed, ExitedOrderly,
                             IpcLostAlive, TerminatedExternally, Unknown };
enum class HelperReadFailure { None, Eof, IoError, LineTooLong, ParseError };
struct HelperExitFacts {            // filled by ZoomEngineProcessClient (Task 4)
  bool haveProcess = false, exited = false, aliveAfterReadFailure = false;
  std::optional<std::uint32_t> exitCode;
  std::optional<std::int64_t> startedAtUnixMs, exitedAtUnixMs;
  std::uint32_t pid = 0;
  bool terminatedByCore = false; std::string terminationContext;
  HelperReadFailure readFailure = HelperReadFailure::None;
  std::uint32_t readWin32Error = 0, readBytes = 0, parseFailures = 0;
};
struct HelperRecorderView {         // decoded from the section (Task 2)
  bool available = false; std::string unavailableReason;
  std::optional<std::string> trapKind; std::uint32_t trapCode = 0; std::uint64_t trapAddress = 0;
  std::string trapModule; std::uint64_t trapRva = 0; std::uint32_t trapThread = 0;
  std::optional<std::int64_t> trapUnixMs, vehUnixMs, lastHeartbeatUnixMs, lastRingUnixMs;
  std::uint32_t vehCode = 0; std::string vehModule; std::uint64_t vehRva = 0;
  std::optional<std::string> orderlyReason; std::string faultInjected;
  std::vector<std::string> lastTags;  // newest last, at most 8
  std::uint32_t invalidParameterCount = 0;
};
struct HelperDumpInventory { std::optional<std::string> trap, wer, zoomDir, trapDumpError; };
struct HelperCommitSample { std::uint64_t used = 0, limit = 0; std::int64_t observedAfterExitMs = 0; };
struct HelperExitVerdict { HelperExitClass cls; std::string classification; std::string causeLine; };

const char* ntStatusName(std::uint32_t code);           // nullptr when unlisted
HelperExitVerdict classifyHelperExit(const HelperExitFacts&, const HelperRecorderView&,
                                     const HelperDumpInventory&, const std::optional<HelperCommitSample>&,
                                     std::int64_t nowUnixMs);
}
```

- [ ] **Step 1: Write the failing tests first.** Write one test per rule 1-10, plus the precedence cases, plus the honesty cases:

```cpp
#include <gtest/gtest.h>
#include "modules/ZoomHelperExitClassifier.h"
using namespace corevideo::modules;

static HelperExitFacts exitedWith(std::uint32_t code) {
  HelperExitFacts f; f.haveProcess = true; f.exited = true; f.exitCode = code; f.pid = 55164;
  f.startedAtUnixMs = 1'000'000; f.exitedAtUnixMs = 1'050'000; return f;
}

TEST(ZoomHelperExitClassifier, ZoomCrashDirWithExitCodeOneIsACrashNotAnExternalKill) {
  HelperRecorderView r; r.available = true;
  HelperDumpInventory d; d.zoomDir = "C:/Users/x/AppData/Roaming/ZoomSDK/logs/zoomcrash_094420_55164";
  const auto v = classifyHelperExit(exitedWith(1), r, d, std::nullopt, 1'060'000);
  EXPECT_EQ(v.classification, "crashed");
  EXPECT_NE(v.causeLine.find("Zoom SDK crash handler wrote"), std::string::npos);
  EXPECT_EQ(v.causeLine.find("terminated externally"), std::string::npos);
}

TEST(ZoomHelperExitClassifier, PlainCodeOneWithNoEvidenceIsTerminatedExternally) {
  HelperRecorderView r; r.available = true; r.lastTags = {"out:meeting_status", "in:subscribe"};
  r.lastHeartbeatUnixMs = 1'049'000;
  const auto v = classifyHelperExit(exitedWith(1), r, {}, std::nullopt, 1'060'000);
  EXPECT_EQ(v.classification, "terminated_externally");
  EXPECT_NE(v.causeLine.find("terminated externally, code 1"), std::string::npos);
  EXPECT_NE(v.causeLine.find("lastHeartbeatAge=1.0s"), std::string::npos);
  EXPECT_NE(v.causeLine.find("in:subscribe"), std::string::npos);
}

TEST(ZoomHelperExitClassifier, CoreTerminateCodeNeverReadsAsACrashEvenIfFlagLost) {
  const auto v = classifyHelperExit(exitedWith(0xC0DE0001u), {}, {}, std::nullopt, 1'060'000);
  EXPECT_NE(v.classification, "crashed_unattributed");
}

TEST(ZoomHelperExitClassifier, MissingFactsAreUnknownAndListWhatWasCaptured) {
  HelperExitFacts f; f.pid = 42;  // no handle, no code
  HelperRecorderView r; r.available = false; r.unavailableReason = "section not opened";
  const auto v = classifyHelperExit(f, r, {}, std::nullopt, 0);
  EXPECT_EQ(v.classification, "unknown");
  EXPECT_EQ(v.causeLine.rfind("unknown: here is what was captured:", 0), 0u);
  EXPECT_NE(v.causeLine.find("exitCode=absent"), std::string::npos);
  EXPECT_NE(v.causeLine.find("flightRecorder=unavailable: section not opened"), std::string::npos);
}
```

Also cover:
- Rule 1 beats a trap stamp.
- Rule 2 for `uef` (`ACCESS_VIOLATION`, module+RVA) and for `abort`.
- Rule 4 (WER dump).
- Rule 5 requires no heartbeat or ring entry after `vehUnixMs`; a later heartbeat drops it to rule 6 or 9.
- Rule 6 for `0xC0000409` and for an unlisted `0xC0001234` printed as `NTSTATUS 0xC0001234`.
- Rule 7 (orderly).
- Rule 8 (`ipc_lost_alive` with `line_too_long 70123 bytes`).
- `faultInjected` appears as `(fault injected: av_write)`.
- Commit renders as `commit=121.7/127.7 GiB (observed +350 ms after exit)`.

- [ ] **Step 2:** Build and run `--gtest_filter='ZoomHelperExitClassifier.*'`. The expected RED is a link/compile failure, then failures once stubbed. Paste the output.
- [ ] **Step 3:** Implement. Keep the rule list in one ordered function, and the NTSTATUS table as a `constexpr` array. The cause-line suffix is built by one helper that writes `absent` for each missing optional.
- [ ] **Step 4:** Run GREEN, then the whole `ZoomEngine*` suites one at a time.
- [ ] **Step 5:** Commit: `Zoom helper exit: pure classifier and NTSTATUS names (#856)`.

---

### Task 2: The flight recorder layout

**Files:**
- Create: `native/zoom-engine/shared/helper-flight-recorder.h` (header-only, no SDK, no STL beyond `<atomic>`/`<cstdint>`/`<cstring>`)
- Create: `native/tests/HelperFlightRecorderTest.cpp`
- Modify: `native/CMakeLists.txt` (test registration)

**Interfaces:**
- Produces:
  - `struct HelperFlightRecorder` (POD, `kMagic = 'CVZH'`, `kVersion = 1`, exactly 65536 bytes, `static_assert`ed).
  - `void hfr_ring(HelperFlightRecorder*, char dir, const char* tag)`
  - `void hfr_heartbeat(HelperFlightRecorder*)`
  - `void hfr_stage(HelperFlightRecorder*, HfrStage)`
  - `void hfr_orderly(HelperFlightRecorder*, const char* reason, uint32_t code)`
  - `bool hfr_snapshot(const volatile HelperFlightRecorder*, HelperFlightRecorder& out)`, which detects a torn read via begin/end sequence counters around trap and orderly writes.
  - `HelperRecorderView decodeRecorder(const HelperFlightRecorder&)`. It lives in the core (`ZoomHelperExitClassifier.cpp`), not in the shared header.
  - `bool hfr_tag_from_line(const char* line, size_t len, char* out, size_t cap)` extracts `cmd`, and `cmd/stage` for `debug`, and **never** any other field.
  - Section/event name builders: `hfr_section_name(token)`, `hfr_trap_request_name(token)`, `hfr_trap_ack_name(token)`, all prefixed `Local\CoreVideoZoomHelperDiag_`.

- [ ] **Step 1: Failing tests first.**
  - Size and offset `static_assert`s mirrored as runtime `EXPECT_EQ` so a layout change fails loudly.
  - The ring wraps at 256 and `decodeRecorder` returns the newest 8 in order.
  - `hfr_tag_from_line` on `{"cmd":"debug","stage":"join_received","meeting_id":"123"}` yields `debug/join_received` and the output contains no digit of `123`.
  - On `{"cmd":"meeting_status","status":3}` it yields `meeting_status`.
  - A line over 40 characters truncates safely.
  - A torn trap write (begin != end) makes `hfr_snapshot` return false.
  - Names are token-scoped and differ for two tokens.
- [ ] **Step 2:** RED, then implement, then GREEN. Commit: `Zoom helper flight recorder: shared fixed layout (#856)`.

---

### Task 3: Typed IPC read failures (implements D2)

**Files:**
- Modify: `native/zoom-engine/shared/engine-ipc.h` (add `IpcReadResult ipc_read_line_ex(IpcFd, std::string&, size_t max_len = 65536)`; keep `ipc_read_line` as a wrapper so the helper side is unchanged)
- Modify: `native/src/modules/ZoomEngineProcess.h/.cpp` (`readEvent()` keeps its signature for the fake client but records `lastReadFailure_`, `lastReadWin32Error_`, `lastReadBytes_`, `parseFailures_`; add `virtual HelperReadFailure lastReadFailure() const`)
- Modify: `native/src/modules/ZoomEngineRuntime.cpp` `readerLoop`
- Test: `native/tests/IpcReadLineExTest.cpp` (new), `native/tests/ZoomEngineRuntimeTest.cpp` (extend `FakeZoomEngineProcessClient` with a scripted failure)

**Interfaces:**
- `enum class IpcReadStatus { Ok, Eof, IoError, TooLong }`
- `struct IpcReadResult { IpcReadStatus status; uint32_t win32Error; size_t bytes; }`
- On `TooLong`, `ipc_read_line_ex` **drains to the next `\n`** before returning, so the stream stays framed. Without draining, skip-and-continue would read garbage.

- [ ] **Step 1: Failing tests.**
  - `IpcReadLineEx` over `CreatePipe`: a normal line gives `Ok`; a 70,000-byte line followed by `{"cmd":"ping"}\n` gives `TooLong(70000)`, then `Ok` with the ping.
  - Closing the write end gives `Eof` (`ERROR_BROKEN_PIPE` maps to `Eof`).
  - Runtime: a fake client that yields one parse failure and then a valid event leaves `meetingState` `in_meeting` and increments a `protocolErrors` warning counter (D2 = skip).
  - 20 consecutive parse failures retire the helper with `readFailure=parse_error` in the warning.
  - The existing `LostPipeReportsDisconnectionWhileProcessStillRuns` and `DeadHelperReportsLossInsteadOfReturningSilently` still pass.
- [ ] **Step 2:** RED, then implement. In `readerLoop`, a `nullopt` with `lastReadFailure()` of `ParseError` or `LineTooLong` takes the D2 branch. Any other failure takes the loss branch, which Task 7 completes; until then it keeps today's message and logs `lastError()` plus the failure kind. Log a protocol-error line once per 5 s at most, with a redacted 200-character prefix (`SupportBundleLogRedactor`-equivalent: drop anything after `"jwt"`, `"zak"`, `"psw"`, `"passcode"`, `"token"` keys).
- [ ] **Step 3:** GREEN across the `ZoomEngineRuntime.*`, `ZoomEngineClient.*` and `IpcReadLineEx.*` suites. Commit: `Zoom helper IPC: distinguish EOF, I/O error, oversized and unparseable lines (#856)`.

---

### Task 4: Exit facts and the diagnostic section in the process client

**Files:**
- Modify: `native/src/modules/ZoomEngineProcess.h/.cpp`
- Test: `native/tests/ZoomEngineProcessTest.cpp`

**Interfaces:**
- Produces:
  - `HelperExitFacts exitFacts() const`
  - `void* duplicateProcessHandleForEvidence()`: the caller owns it; Windows only; returns nullptr elsewhere.
  - `const HelperFlightRecorder* recorder() const`
  - `void* trapRequestEvent() const`, `void* trapAckEvent() const`
  - `void noteTerminationContext(const char*)`
- Free function for testing: `HelperExitFacts captureExitFactsForHandle(void* process, bool terminatedByCore, const char* context)`.
- Constant: `constexpr std::uint32_t kCoreTerminateExitCode = 0xC0DE0001;`
- Command line: `"<exe>" --ipc-token T --diag-section <name> --diag-trap-request <name> --diag-trap-ack <name>`.

- [ ] **Step 1: Failing tests.**
  - `captureExitFactsForHandle` on a real child `cmd.exe /c exit 7`, created with `CreateProcessW` in the test, gives `exitCode == 7`, `exited`, `startedAtUnixMs <= exitedAtUnixMs` and `!terminatedByCore`.
  - On a child the test kills with `TerminateProcess(h, 1)` it gives `exitCode == 1`.
  - On a child killed via a client-style terminate it gives `0xC0DE0001` and `terminatedByCore`.
  - `buildZoomEngineCommandLine(exe, token, names)` contains all four flags, quoted correctly.
  - The section created for token `T` has size 65536 and the header magic set by the core. The core writes `magic`, `version` and `helperPid = 0`; the helper fills in its PID.
- [ ] **Step 2:** Implement.
  - **Before** `CreateProcessA`, create the section (`CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, 65536, name)`) and both auto-reset events. If any creation fails, log it and continue without diagnostics: the recorder view then reports `unavailable: <win32>`. Never fail the join because diagnostics failed.
  - `stop()` and `terminate()` set the termination flag, then `TerminateProcess(h, kCoreTerminateExitCode)`, then cache `exitFacts` **before** `closeHandle(processHandle_)`.
  - `running()` caches the exit code when it first sees one.
  - `start()`'s connect-timeout and superseded paths call `noteTerminationContext("connect_timeout")` or `("superseded")` first.
  - The section and events stay owned by the client until its destructor. The collector holds its own `DuplicateHandle` of the section (Task 7), so it outlives a Rejoin.
- [ ] **Step 3:** GREEN. The POSIX branch only populates `exitCode`/`exited` from the `waitpid` status it already reads (compile-only; note "never run" in the commit body). Commit: `Zoom helper process: keep exit code, process times, and who terminated it (#856)`.

---

### Task 5: The crash trap and recorder hooks in the helper

**Files:**
- Create: `native/zoom-engine/shared/engine-crash-trap.h`, `native/zoom-engine/shared/engine-crash-trap.cpp`
- Modify: `native/zoom-engine/engine/engine-writer.h` (`EngineIpc::write` rings the tag; `ping` calls `hfr_heartbeat`)
- Modify: `native/zoom-engine/engine/main.cpp`. Install the trap first in `main()`. Ring each inbound command. Set the stage at the init/auth/join/leave/cleanup points. Re-assert hooks after `InitSDK` returns and on `in_meeting` in `onMeetingStatusChanged`. Write orderly-exit stamps at the `return` sites. Change `return 1` to `return 0xC701` for the IPC setup failure.
- Modify: `native/zoom-engine/engine/engine-video.cpp`, `engine-share.cpp` (one `hfr_ring('s', "...")` in `onRawDataStatusChanged`, `onSharingStatus`)
- Modify: `native/zoom-engine/fake/fake-engine.cpp` (install the trap; heartbeat and ring through the same calls)
- Modify: `native/CMakeLists.txt`. Add `engine-crash-trap.cpp` to `corevideo-zoom-engine` and `corevideo-zoom-engine-fake`. Link `dbghelp` only into `corevideo_native` (Task 7), never into the helper.
- Test: `native/tests/EngineCrashTrapTest.cpp` (pure parts only)

**Interfaces:**
- `bool crash_trap_install(int argc, char** argv)` opens the section and events named on the command line. It falls back to a static buffer and records why.
- `void crash_trap_reassert(const char* point)`
- `HelperFlightRecorder* crash_trap_recorder()`
- `void crash_trap_set_fault_label(const char*)` (used by Task 6)

- [ ] **Step 1: Failing tests (pure, in-process, no crash).**
  - `crash_trap_parse_args` finds the three names, in either `--flag value` or `--flag=value` form.
  - `crash_trap_module_of(address)` returns the test exe's module name for a function in the test.
  - `crash_trap_is_fatal_code` is true for exactly the six VEH codes in the spec.
  - With no args, the static-buffer fallback reports `unavailableReason == "no --diag-section"`.
  - **Do not** raise real exceptions in the test process; Task 7's drill does that in the fake.
- [ ] **Step 2:** Implement the handlers per spec §2.
  - The UEF stamps the trap record, `SetEvent(trapRequest)`, `WaitForSingleObject(trapAck, 20000)`, then chains to the saved previous filter or returns `EXCEPTION_CONTINUE_SEARCH`.
  - The VEH is registered with `AddVectoredExceptionHandler(0, ...)`. It stamps only fatal codes, only the first one per 100 ms, and always returns `EXCEPTION_CONTINUE_SEARCH`.
  - `SIGABRT` stamps the trap, requests the dump, waits for the ack, then calls the saved previous handler if it is neither `SIG_DFL` nor `SIG_IGN`.
  - Purecall stamps the trap and calls `abort()`.
  - Invalid-parameter follows D1: increment the count, stamp the time, and return for absorb, or for fail call `TerminateProcess(GetCurrentProcess(), 0xC702)` after a dump request.
  - Re-assertion records the module of each replaced handler into `handlerChain`. Skip re-installing when the current handler is already ours.
- [ ] **Step 3:** Build the real helper (`ZOOM_SDK_DIR` set) and the fake in Release. Run the pure tests GREEN. Smoke test: launch the fake by hand with a test section created by a tiny PowerShell P/Invoke, or wait for Task 7's drill, and confirm the heartbeat advances.
- [ ] **Step 4:** Commit: `Zoom helper: crash trap and flight recorder hooks (#856)`.

---

### Task 6: Fault injection, double-gated

**Files:**
- Modify: `native/zoom-engine/shared/engine-crash-trap.h/.cpp` (`FaultSpec parse_fault_spec(const char*)`; `bool fault_armed()`; `[[noreturn]] void fire_fault(FaultKind)`)
- Modify: `native/zoom-engine/engine/main.cpp`, `native/zoom-engine/fake/fake-engine.cpp`. Handle `{"cmd":"diag_fault","kind":"..."}`. Arm the timed trigger B on `in_meeting`; the fake treats "joined" as `in_meeting`.
- Test: `native/tests/EngineCrashTrapTest.cpp` (extend)

**Interfaces:**
- Kinds: `av_write | throw_bad_alloc_thread | abort | purecall | invalid_parameter | fastfail | stack_overflow | exit_code_7 | hang`.
- `COREVIDEO_ZOOM_ENGINE_FAULT_ARM=crash-this-helper` is required. `COREVIDEO_ZOOM_ENGINE_FAULT=<kind>@in_meeting+<seconds>` is optional.

- [ ] **Step 1: Failing tests.**
  - `parse_fault_spec("av_write@in_meeting+30")` gives `{AvWrite, 30}`.
  - Garbage gives `invalid`.
  - `fault_armed()` is false for an unset arm variable, false for `1`, `true` or `yes`, and true only for the exact sentence.
  - The `diag_fault` handler with arming absent writes `{"cmd":"debug","stage":"fault_injection_ignored"}` and does **not** fire. Test this through a seam function `handle_diag_fault_line(line, armed, firer)` with a recording firer.
- [ ] **Step 2:** Implement. When armed, the helper records `faultInjected` in the section and sends `{"cmd":"debug","stage":"fault_injection_armed","kind":"..."}` before firing. `hang` stops the heartbeat and the read loop but keeps the process alive.
- [ ] **Step 3:** GREEN. Commit: `Zoom helper: double-gated fault injection for exit diagnostics (#856)`.

---

### Task 7: The evidence collector, runtime integration and snapshot fields

**Files:**
- Create: `native/src/modules/ZoomHelperExitEvidence.h/.cpp`. This holds the collector thread, the dump inventory, the commit sample and the JSON record writer. Filesystem roots and the clock are injectable for tests.
- Modify: `native/src/modules/ZoomEngineRuntime.h/.cpp`:
  - A `lastHeartbeatSteady_` set on `Ping`.
  - Handing jobs to the collector from `readerLoop` and `ensureStarted`. On spawn the collector starts the trap watch; on loss the reader reports.
  - The `reportEngineLossLocked(message, provisionalVerdict)` cause text.
  - `helperExitJson()` and `helperLiveJson()` accessors.
  - A bounded history of 8.
- Modify: `native/src/core/CoreStateCapture.cpp` (add `helper`, `helperExit` and `helperExitHistory` to the `zoom` object at both sites, and to `rawCaptureSnapshotLocked`)
- Modify: `native/CMakeLists.txt` (`dbghelp` for `corevideo_native` on WIN32; new sources and tests)
- Test: `native/tests/ZoomHelperExitEvidenceTest.cpp` (unit), `native/tests/ZoomHelperExitDrillTest.cpp` (spawns `corevideo-zoom-engine-fake.exe` from the test binary's directory; skips loudly with `[  SKIPPED ]` if it is absent)

**Interfaces:**
- `class ZoomHelperExitEvidence`:
  - `startWatch(HelperWatchJob)` and `reportLoss(HelperLossJob)` never block.
  - A publish callback `std::function<void(HelperExitRecord)>` is invoked off-lock. The runtime's implementation takes `mutex_` only to swap.
  - `shutdown()` is bounded at 2 s and is called from the runtime destructor **after** `stopReader()`.
- Roots: `crashDir` (`COREVIDEO_CRASH_DIR`, else `%LOCALAPPDATA%\CoreVideoPro\crashes`), `legacyWerDir` (`%LOCALAPPDATA%\CrashDumps`) and `zoomCrashRoots`. `zoomCrashRoots` is one constant list starting with `%APPDATA%\ZoomSDK\logs`; Task 8 confirms or corrects it.
- Record file: `<crashDir>\zoom-helper-exit-<yyyyMMddTHHmmssZ>-<pid>.json`, written via temp file and `MoveFileExW(REPLACE_EXISTING)`.

- [ ] **Step 1: Failing unit tests (no processes).**
  - With an injected clock and temp dirs, a loss job whose facts say code 1, and a Zoom dir `zoomcrash_094420_<pid>` created **at the +3 s scan**, publishes `terminated_externally` first and then `crashed` with `evidenceComplete:false`, then `true` after the +30 s scan. The test advances the fake clock; no real sleeps over 100 ms.
  - A dir for a different PID, or one outside the time window, is ignored.
  - A queue overflow (5 jobs) drops one and counts it.
  - An unwritable `crashDir` gives `recordPath: "unwritten: ..."` and the record is still published.
  - The publish callback is never invoked while the test holds a mutex it hands to the collector. Use a `try_lock` probe that asserts.
- [ ] **Step 2: Failing drill tests (Windows, real processes, the fake helper).** Each case spawns the fake through `ZoomEngineRuntime` with `COREVIDEO_ZOOM_ENGINE_PATH` and a temp `COREVIDEO_CRASH_DIR`. The arm variable is set in the test process environment, which the helper inherits. Each case waits at most 40 s for `evidenceComplete`.

| Case | Expected |
|---|---|
| `diag_fault av_write` | `crashed`, `trap.kind=="uef"`, code `0xC0000005`, a `zoom-helper-trap-*.dmp` larger than 0 bytes, `(fault injected: av_write)` |
| `diag_fault abort` | `crashed`, `trap.kind=="abort"` |
| `diag_fault fastfail` | `crashed_unattributed` or `crashed` via WER if the runner has LocalDumps; assert one of the two, and that the cause names `0xC0000409` |
| Test calls `TerminateProcess(pid, 1)` | `terminated_externally`, `code 1`, last tags non-empty, `lastHeartbeatAgeMs` present |
| `quit` via `leave` + runtime shutdown | `terminated_by_core` or `exited_orderly` (assert which, and that it is not externally) |
| `diag_fault hang` | `zoom.helper.heartbeatAgeMs` grows past 5000 while there is no `helperExit` |
| After each case | `join()` with the fake starts a fresh helper on the first attempt, and no extra helper process exists (enumerate by image name before and after) |

- [ ] **Step 3: Implement** per spec §3.3. The trap watch thread waits on `{trapRequest, processDup}`. Dump flags follow D4. Set `TrapAck` in all paths. `GetPerformanceInfo` comes via `psapi`. The re-scan schedule is +0, +3, +10 and +30 s. Each publish also writes `[zoom-helper-exit] <causeLine> record=<path>` via `nativeLogf`.
- [ ] **Step 4:** GREEN for unit and drill tests, then the full native suite. Record the pass count and compare it with Task 0's.
- [ ] **Step 5:** Commit: `Zoom helper exit: evidence collector, out-of-process trap dump, snapshot fields (#856)`.

---

### Task 8: Real-SDK fault test (local Windows, `ZOOM_SDK_DIR`)

**Files:**
- Create: `native/tests/ZoomHelperRealSdkFaultTest.cpp` (gated: runs only when `COREVIDEO_REAL_ZOOM_ENGINE_PATH` is set, otherwise prints `[  SKIPPED ]`)
- Modify: `native/src/modules/ZoomHelperExitEvidence.cpp` (`zoomCrashRoots` if the measured root differs)
- Modify: the spec's new `## Outcome` section (findings only)

**Interfaces:**
- Consumes the real `corevideo-zoom-engine.exe` from `native/build-dev/`. Sends `init` with an empty JWT, which makes `InitSDK` install Zoom's handlers; auth fails, which is fine. Then sends `diag_fault`.

- [ ] **Step 1:** Write the test.
  - For `av_write`, `throw_bad_alloc_thread`, `abort` and `invalid_parameter`, it asserts the classification, records `handlerChain` (which module owned the UEF and `SIGABRT` before our re-assertion), and records which dumps appeared: ours, Zoom's, WER's.
  - For `invalid_parameter`, it asserts the D1 behaviour: absorbed, meaning the process is still alive with count 1; or fail, meaning `crashed` with `0xC702`.
- [ ] **Step 2:** Run it with `COREVIDEO_CRASH_DIR=D:\CoreVideo-QA\evidence\zoom-helper-exits\task8-<sha>`. Confirm the real `zoomcrash_*` root on this box and fix the constant if needed (its unit test from Task 7 is updated with it).
- [ ] **Step 3: Decision point.** If Zoom's handler terminates the process **before** our chained filter or `SIGABRT` hook runs, record that, because then the trap dump never appears. Rules 3 and 5 must still classify `crashed`, and the test asserts this. If they do not, stop and report `BLOCKED` with the evidence; the design needs revisiting.
- [ ] **Step 4:** Write the outcome (SHA, findings, dump sizes) into the spec. Commit: `Zoom helper exit: real-SDK handler-chain evidence (#856)`.

---

### Task 9: Shell surfacing, support bundle, scanner directories (implements D3)

**Files:**
- Create: `native-shell/CoreVideoPro.MediaCore/Services/ZoomHelperExitObserver.cs` (pure: snapshot JSON in, log lines out, deduplicated on `(pid, generation, evidenceComplete)`)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/CrashDumpScanner.cs` (`CrashDumpDirectories()` returns `COREVIDEO_CRASH_DIR` or `%LOCALAPPDATA%\CoreVideoPro\crashes`, plus the legacy `%LOCALAPPDATA%\CrashDumps`; `Scan` accepts a list. Per D3, `corevideo-zoom-engine.exe` dumps and `zoom-helper-trap-*.dmp` are listed but excluded from `CrashReportCoordinator`'s upload set.)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/CrashReportCoordinator.cs` (use the directory list and the D3 filter)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/SupportBundleArchiveBuilder.cs`. Add `HelperExitRecordsDirectory`, `ZoomCrashRoots` and `IncludeResourceExhaustionEvents`. Add `helper-exits/` entries (10 newest, ≤64 KiB each, redactor applied). `dumps.txt` gains the multi-directory and Zoom-dir listing. Add `exit-context.txt` (event 2004 entries ±10 min of each record's `exitedAtUtc`, read via `System.Diagnostics.Eventing.Reader`; on access failure, add a skip note).
- Modify: the shell's snapshot-apply site in `MediaCoreSupervisor` or the bridge (whichever already receives each core snapshot; find it with `grep -n "zoomRosterEpoch\|ApplySnapshot" native-shell/CoreVideoPro.MediaCore/Services/*.cs`). It calls the observer and writes its lines with `LaunchLog.Write`.
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/ZoomHelperExitObserverTests.cs`, `CrashDumpScannerTests.cs` (extend), `native-shell/CoreVideoPro.WinUI.Tests/SupportBundleArchiveTests.cs` (extend)

- [ ] **Step 1: Failing tests.**
  - Observer: one provisional and one final line for the same exit. A repeated snapshot adds nothing. A new generation gives a new line. An absent field prints `absent`. A record from an older `rosterEpoch` than the current one is logged but **never** surfaces as current.
  - Scanner: a dump in `crashes` is found; the legacy dir is still found; a helper dump is listed but not in the upload set.
  - Bundle:
    - A temp `crashes` dir with 12 records includes the newest 10.
    - A 100 KiB record is truncated with a manifest note.
    - A `zoomcrash_x_123` dir appears in `dumps.txt` by name only, and no `.dmp` is ever packed: assert that no zip entry ends in `.dmp`.
    - With event-log access denied, `exit-context.txt` gets a skip note and the export still succeeds.
- [ ] **Step 2:** RED, then implement, then GREEN: `dotnet test native-shell/CoreVideoPro.MediaCore.Tests` and the WinUI test project filtered to the touched classes.
- [ ] **Step 3:** Commit: `Shell: Zoom helper exit lines, bundle records and listings, crash dir fix (#856)`.

---

### Task 10: QA drill script and installed acceptance

**Files:**
- Create: `scripts/qa/zoom-helper-exit-drill.ps1`
- Create (evidence, not committed): `D:\CoreVideo-QA\evidence\zoom-helper-exits\<run-id>\`

**Interfaces:**
- Consumes the control API (`127.0.0.1:8011`, `GET /snapshot`), the shell's support-bundle export command (find the existing control route or UIA Invoke; **no input injection**), and `taskkill`.
- `-Mode fake|installed`, `-Kind av_write|throw_bad_alloc_thread|external_kill`, `-OutputDirectory` (must be new and empty, as in `engine-host-crash-drill.ps1`).

- [ ] **Step 1: Write the script.**
  - It refuses a non-empty output directory and records binary SHA-256s.
  - For SDK kinds it requires the arm variable to be set **by the operator** before the app was launched; the script never sets it machine-wide.
  - It polls `/snapshot` until `zoom.helperExit.evidenceComplete`, at most 60 s.
  - It verifies the record file, the dump(s) named in it, both `launch.log` lines and a fresh support bundle zip containing `helper-exits/` and the dump listing.
  - It then measures Rejoin: issue the existing Rejoin via the control API, time it to `in_meeting`, and confirm exactly one `corevideo-zoom-engine.exe` exists.
  - It prints `PASS` / `FAIL` / `MISSING_EVIDENCE` per acceptance item 1-5.
- [ ] **Step 2:** Run `-Mode fake` on the dev box for all three kinds. Attach the verdicts.
- [ ] **Step 3: Installed acceptance** (owner present; authorized test meeting; full pwd link).
  1. Install the build carrying this work (an owner-run elevated step in an owner-scheduled window, never over the owner's beta during the soak; UAC on the dev PC has EnableLUA=1 with prompts suppressed, and this must also work on a normal PC) and record the Rejoin baseline (three runs) **before** arming.
  2. Close the app. Set `COREVIDEO_ZOOM_ENGINE_FAULT_ARM=crash-this-helper` and `COREVIDEO_ZOOM_ENGINE_FAULT=av_write@in_meeting+30` **process-locally in the launch harness** (the drill script sets them in the environment of the child process it starts), never as a persistent user or machine environment variable (D21: allowed on the same terms as D11, never set by the installer). Launch and join.
  3. Run the drill. Repeat with `throw_bad_alloc_thread`.
  4. Relaunch without the variables (they were process-local to the previous launch), join, and run `-Kind external_kill`.
  5. **Confirm with `Get-ChildItem Env:` in a fresh shell that neither variable exists** (they must never have been persisted). Then confirm a normal join works.
- [ ] **Step 4:** Post a redacted results comment on #856 (verdicts, classifications, cause lines; no paths that contain user names, no dumps). Raw evidence stays on `D:`.

---

### Task 11: Documentation

**Files:**
- Create: `docs/reference/zoom-helper-exit-diagnostics.md`. Cover what each classification means, where evidence lands, how to read a record, the fault-injection variables (QA only), and the October 10/11 signature (heartbeat gap, then a Zoom dump).
- Modify: `CLAUDE.md` (one row in the reference table: "Zoom helper crashes/exits, exit evidence, helper fault injection")
- Modify: `docs/reference/fault-injection-seams.md` (the stated exception for the helper's env-armed seam)
- Modify: `docs/reference/zoom-capture-on-off.md:83` (point to the new topic; the Zoom crash root as measured in Task 8)
- Modify: `docs/reference/gotchas.md`. Add three gotchas: the exit code is read only once per handle; code 1 is ambiguous without the flight recorder; dumps appear seconds after exit, so re-scan.
- Modify: `docs/reference/realtime-isolation-audit.md` (the "SDK callbacks / recovery" row gains "independent exit evidence: shipped")
- Modify: `docs/BACKLOG.md` (the #856 row: diagnostics shipped, natural-recurrence acceptance still open)
- Modify: the spec's Outcome section (final)

- [ ] **Step 1:** Write the docs. **Step 2:** Commit: `Docs: Zoom helper exit diagnostics (#856)`.

---

## Self-review

- **Spec coverage.**
  - L1 and L5 map to Task 4; L2 and L3 to Tasks 3 and 7; L4 to Task 3; L6 to Tasks 5 and 7; L7 to Task 5; L8 to Task 9.
  - Spec §1 (recorder) maps to Tasks 2, 4 and 5; §2 (trap) to Task 5; §3.1 to Task 4; §3.2 to Tasks 3 and 7; §3.3 to Task 7; §3.4 to Task 1; §4 to Tasks 7 and 9; §5 to Task 6; §6 to Global Constraints and Review Focus 3-4.
  - Acceptance 1-5 is covered by Tasks 7, 8 and 10; acceptance 6 by Tasks 1 and 7.
  - D1-D4 are recorded in Task 0 and applied in Task 5 (D1), Task 3 (D2), Task 9 (D3) and Task 7 (D4).
- **Placeholders.**
  - Task 9's snapshot-apply site is located by a named grep, because two candidate classes exist.
  - Task 8's Zoom crash root is measured, not assumed.
  - Every other step names its files, interfaces and assertions.
- **Type consistency.**
  - `HelperExitFacts`, `HelperRecorderView`, `HelperDumpInventory` and `HelperExitVerdict` (Task 1) are produced by Task 4 (`exitFacts()`), Task 2 (`decodeRecorder`) and Task 7 (inventory), and consumed by Task 7.
  - The snapshot field names in Task 7 are the ones Task 9's observer parses.
  - `kCoreTerminateExitCode` (Task 4) is the `0xC0DE0001` that Task 1's table names.
