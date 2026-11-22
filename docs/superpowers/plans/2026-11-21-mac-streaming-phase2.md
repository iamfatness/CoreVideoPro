# macOS dependable streaming, Phase 2 (F1, P1, D1) implementation plan

Status: executing, 2026-11-21. Branch `mac-streaming-phase2` from `fc675541` (Mac Phase 1, PR #1220).

> **For agentic workers:** `docs/superpowers/plan-authoring-rules.md` is binding. Every task names the
> mutations that must turn its tests red, and its report says whether each did. When a supplied test
> contradicts a reasonable implementation, suspect the test first.

**Goal:** a fresh Mac with no Homebrew streams to RTMP/RTMPS, SRT and HLS using an ffmpeg we build,
sign and ship; no ffmpeg or helper child outlives the media core, even when the core is SIGKILLed; and
the operator sees one row per destination whose status is decided from the core's lifecycle and
supervisor evidence (never "live" from a launch), with Retry for a destination that stopped retrying.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-outputs-ingest-browser-design.md` §5.2, slices F1, P1, D1.
Sequencing: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md` Phase 2 (owner decision
2026-11-18: bundle LGPL ffmpeg).

**Architecture in one paragraph.** Nothing about streaming policy moves. The shared RTMP/SRT/HLS sender
(`native/src/modules/RtmpOutputSenderAdapter.cpp`, one class, three protocol profiles), the output
supervisor and the truthful lifecycle are platform-neutral and already compiled into the Mac release
core. Three gaps remain. (F1) The sender launches an external `ffmpeg` the package does not contain; we
build a static, LGPL, arm64 ffmpeg from three SHA-256-pinned tarballs and ship it at
`Contents/Resources/ffmpeg`, which the sender already searches first after its environment overrides
(it looks in its own executable's directory; the release core lives in `Contents/Resources`). (P1) On
POSIX nothing ties an ffmpeg child to the core's life, and every child inherits every other child's pipe
ends; a new `posix_child` spawn helper starts each child through a tiny `corevideo-child-guard` that
holds the read end of a process-wide "lifeline" pipe whose write end only the core holds, kills its
child at EOF, and mirrors the child's exit status; the helper also puts each child in its own process
group and closes every descriptor it did not name. (D1) The SwiftUI shell grows from one hard-wired
RTMP form to three destination rows (RTMP/RTMPS, SRT, HLS; the core has exactly one sender per protocol
id), a pure status projection over `outputSenderSession.senders[]` lifecycle + supervisor, and Retry
(`recover-output-sender`).

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks that change if overridden |
|---|---|---|---|
| D1 | **ffmpeg is a pinned SOURCE build, not a pinned prebuilt.** ffmpeg 8.1.3 + OpenSSL 3.5.9 (LTS) + libsrt 1.5.7, pinned by URL + SHA-256 in `scripts/macos-ffmpeg.lock`, built by `scripts/build-macos-ffmpeg.sh` into a cache keyed by SHA-256(lock + script). OpenSSL's hash is cross-checked against upstream's published `.sha256`; ffmpeg's and libsrt's are trust-on-first-use (no gpg on this machine for ffmpeg's `.asc`). Every pinned version was released more than two weeks before pinning. | Every prebuilt macOS ffmpeg we could pin is GPL (x264/x265) or links Homebrew dylibs; a source build is the only reproducible LGPL route. The build needs only Xcode CLT, cmake, perl, curl (≈80 s on an M3 Max). | 4, 5 |
| D2 | **Built on the release machine by `release-macos.sh`, not in CI.** | The spec recommends CI; the release already builds locally and signs locally. A CI build is a follow-up. | 5 |
| D3 | **One static executable.** OpenSSL and libsrt are linked statically into `ffmpeg`; no `libav*.dylib`. `--disable-autodetect` plus a pkg-config shim that answers only for those two libraries keeps any Homebrew library out. | One Mach-O to sign; library validation and dylib search paths cannot fail at runtime. | 4, 5 |
| D4 | **Licence posture: LGPL v2.1-or-later, separate process.** No `--enable-gpl`, no `--enable-nonfree` (the audit refuses either and refuses a non-LGPL `-L`). OpenSSL 3 (Apache-2.0) and libsrt (MPL-2.0) are licence-compatible with that. Notices ship at `Contents/Resources/notices/ffmpeg/`: ffmpeg's LICENSE.md and COPYING.LGPL*, OpenSSL's and libsrt's licences, and `PROVENANCE.txt` (versions, URLs, SHA-256s, exact configure lines), mirroring the Windows `notices/ffmpeg/` + provenance file. **No source tarballs ship in the pkg.** | Matches the Windows posture. A written offer of source needs a contact the owner must choose: owner follow-up, recorded in the outcomes. | 5 |
| D5 | **Bundle location and signing:** `Contents/Resources/ffmpeg` (identifier `us.iamfatness.corevideopro.ffmpeg`) and `Contents/Resources/corevideo-child-guard` (identifier `us.iamfatness.corevideopro.child-guard`), both Developer ID, hardened runtime, timestamp, **no entitlements**. A release without either is fatal under `COREVIDEO_REQUIRE_HELPERS=1`. | ffmpeg needs no JIT, no unsigned memory, no foreign libraries; the existing Mach-O audit in `release-macos.sh` then covers both. | 5 |
| D6 | **The child guard is a separate tiny executable**, not a mode of `corevideo-native` (the virtual camera's helper precedent). | It starts in about a millisecond, runs no static initializers from the 4.5 MB core, and the tests exercise the exact shipped binary. | 2, 3, 5 |
| D7 | **Guard grace is 2 s** between SIGTERM and SIGKILL on lifeline EOF. | ffmpeg on SIGTERM tries to write a trailer to a possibly wedged network; 2 s is the AsyncOutputSender grace already used for a wedged writer. | 2 |
| D8 | **Destinations are three fixed rows, one per protocol id (rtmp, srt, hls)**, each with an enable toggle. NDI stays out (its own toggle, slice N1). | The core constructs exactly one sender per protocol id and matches settings by id or protocol (`RtmpOutputSenderAdapter.cpp:334-389`); a second RTMP row would have nowhere to go. | 6, 7, 8 |
| D9 | **"Live" = `lifecycle.state == "producing"` AND `supervisor.healthy`.** The published `status` is never read for the label. A sender with no supervisor shows "Unsupervised" rather than inventing health. | The published `status` still passes the adapter's launch word through while the lifecycle is `preparing` and the supervisor has no failures (`OutputLifecyclePolicy.h:241-242`). | 6 |
| D10 | **Retry is offered exactly when `supervisor.gaveUp`** (the supervisor's terminal and five-strike paths both set it, `OutputDestinationSupervisorPolicy.h:321-338`) and sends `recover-output-sender {destination, reason:"operator-retry"}`. | The documented operator reset; it always clears give-up. | 6, 7 |
| D11 | **SRT passphrase lives in the Keychain** (same service as the RTMP key, account `srt-passphrase`); the HLS URL and every non-secret SRT field live in `ShellPrefs`. | House rule: never persist a secret plaintext. | 7 |
| D12 | **Unchanged:** the stream recipe (H.264, 30 fps, 2 s GOP, 160 kbps), the remote-control `transport.stream.rtmp.configure` semantics, and `transport.stream.srt.configure` staying `blocked` in the generated manifest. | O1 owns profiles; the control-surface spec owns the action registry. | — |
| D13 | **SRT/HLS rows carry the RTMP row's bitrate and codec** (the Windows builder copies the stream profile into each). | Windows parity. | 6 |

---

## Spec corrections (verified against the code, 2026-11-21)

1. The bundle script is `scripts/make-macos-shell-bundle.sh` (the brief and spec say `make-macos-bundle.sh`).
   The `.pkg` (`make-macos-pkg.sh`) is built from the already signed app, so signing in the shell-bundle
   script covers the pkg path.
2. The snapshot node is `outputSenderSession.senders[]` (`CoreStateCapture.cpp:793, 2880`), not
   `outputSenders.senders[]`, and its rows carry **no `redactedEndpoint`** (only the
   `ffmpeg-process-start` event does). D1's row reads `destination`, `status`, `destinationHealth`,
   `lifecycle{state,health,error}`, `supervisor{healthy,gaveUp,consecutiveFailures,nextAttemptInMs,
   lastProgressAgeMs,reason,failureClass}`, `lastResultCode`, `warning`, `runtimeDetail`, `framesSent`.
3. The published `status` is not safe for a "live" label (see D9).
4. Ingest's ffmpeg lookup differs by more than the variable name: it also skips the core's own directory
   (`SrtIngestCaptureAdapter.cpp:98-130`), so the bundled ffmpeg would be invisible to ingest. Task 4
   moves both lookups onto one POSIX resolver.
5. **Pre-existing POSIX defect the spec does not name:** the sender's `posix_spawn` passes no attributes
   and none of its pipes is close-on-exec (`RtmpOutputSenderAdapter.cpp:1949-2027`), so every ffmpeg
   inherits the write ends of every earlier sibling's pipes. A sibling then holds another child's stdin
   open: closing the core's end never delivers EOF, and only the SIGTERM fallback ends it. Task 2's
   helper fixes it with `POSIX_SPAWN_CLOEXEC_DEFAULT`; its test proves the defect by mutation.
6. Terminal failures set `gaveUp` too (D10), so one Retry rule covers both.
7. Windows has **no per-destination supervisor UI or Retry** (no `Supervisor`/`gaveUp` read in
   `CoreVideoPro.WinUI` outside unrelated services). D1's status rows are new UI built on the core
   contract; only the SRT/HLS validation and the settings payload are ports
   (`StudioStreamOutputValidation.ValidateSrt/ValidateHls`, `StudioViewModel.StreamDestinations.cs`).
8. `destinationIsolationTraits` names `rtmp`/`srt` only (`OutputDestinationSupervisor.h:102-112`) and its
   comment says "job object" for every platform. Confirmed; Task 3 adds `hls` and says what POSIX does.
9. CI's `native-metal-macos` job installs Homebrew ffmpeg (`ci.yml:288-293`), so ffmpeg-dependent tests
   run against Homebrew there and against whatever the locator finds here.

---

## Before you start

- **Worktree:** `.claude/worktrees/mac-streaming-phase2`, branch `mac-streaming-phase2`. Never push;
  commit per task with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Native config (CI's `native-metal-macos` flags):**
  `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON -DCOREVIDEO_WITH_AVF_CAPTURE=ON -DCOREVIDEO_WITH_SCK=ON -DCOREVIDEO_WITH_RTMP_OUTPUT=ON -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON`;
  build `cmake --build native/build-metal -j 8`. The test runner is custom: use `--gtest_filter` with a
  single pattern (colon lists are unreliable); there is no `--gtest_repeat`.
- **Shell:** `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`;
  `bash scripts/lint-mac-design.sh` must stay green.
- **Other agents build on this machine** (load average 8-12 observed). Rerun any timing-sensitive
  failure before believing it.
- Baselines are recorded in the outcomes doc.

---

## Task 1: this plan

Commit the plan, the lock file and the ffmpeg build script (the build script is exercised and its audit
proven in Task 4/5; committing it here lets Task 2-3 run in parallel with the first build).

---

## Task 2 (P1, native): `posix_child` spawn helper + `corevideo-child-guard`

**Files:** new `native/src/modules/PosixChildProcess.h` (header-only, POSIX only), new
`native/child-guard/main.cpp` → executable `corevideo-child-guard` (all non-Windows builds), new
`native/tests/PosixChildProbe.cpp` → test executable `corevideo-posix-child-probe`, new
`native/tests/PosixChildProcessTest.cpp`; `native/CMakeLists.txt`.

**Interfaces.**
```cpp
namespace corevideo::modules::posix_child {
inline constexpr int kLifelineChildFd = 9;          // where the guard finds the lifeline
struct FdMapping { int parentFd; int childFd; };     // dup2 parentFd -> childFd in the child
struct SpawnRequest {
  std::string executable;                 // what to run (absolute)
  std::vector<std::string> arguments;     // argv[1..]
  std::vector<FdMapping> fds;             // every descriptor the child gets besides 1/2
  std::string stdoutPath = "/dev/null";
  std::string stderrPath = "/dev/null";
  std::string guardExecutable;            // "" = resolveChildGuard(); still "" = unguarded
};
struct SpawnResult { pid_t pid = 0; bool guarded = false; int error = 0; };
SpawnResult spawn(const SpawnRequest&);        // child leads its own process group (pgid == pid)
void signalGroup(pid_t pid, int signal);       // kill(-pid, signal); no-op for pid <= 0
// SIGTERM to the group, wait up to grace, SIGKILL to the group, then a blocking reap.
// Never called with an adapter mutex held by a caller that a writer thread needs.
bool stopGroup(pid_t pid, std::chrono::milliseconds grace, int* status);
std::string resolveChildGuard();               // $COREVIDEO_CHILD_GUARD_PATH, else beside this executable
int lifelineReadFd();                          // process-wide pipe; write end close-on-exec, never closed
}
```
`corevideo-child-guard --lifeline-fd 9 -- <executable> <args...>`: forks and execs the target (which
keeps fds 0..8 as given), closes its own copies of every descriptor except 2 and the lifeline, then
waits on both. Lifeline EOF → SIGTERM the child, up to 2 s (D7), SIGKILL, reap, exit 125. SIGTERM/SIGINT/
SIGHUP to the guard → forwarded to the child. Child exits with code N → guard exits N; child dies by
signal S → guard resets S to default and raises it on itself, so the core's `waitpid` sees
`WIFSIGNALED/WTERMSIG == S`. Exec failure → exit 127 with a stderr line; bad usage → exit 126.

**Probe modes** (`corevideo-posix-child-probe <mode> [arg]`): `stubborn <pidfile>` (ignores SIGTERM,
never reads stdin, writes its pid, sleeps 120 s); `eof-exit` (reads fd 0 to EOF, exits 7);
`exit <n>`; `fake-core <pidfile> <guard>` (spawns `stubborn <pidfile>` through `posix_child::spawn`
with the given guard, prints the guard pid, then sleeps forever: a stand-in core the test can SIGKILL).

**Tests (`PosixChildProcess.*`, POSIX only), each seen red first:**
1. `ParentSigkillLeavesNoChild`: start `fake-core`, read the stubborn pid from its pidfile, SIGKILL the
   fake core, assert `kill(stubbornPid, 0)` reaches ESRCH within 5 s (2 s grace + slack). Fixture
   invariant: the stubborn child must be alive (pidfile written, `kill(pid,0) == 0`) BEFORE the SIGKILL,
   or the assertion is vacuous; assert that first.
2. `GuardMirrorsExitCodeAndSignal`: `exit 42` reaps as `WEXITSTATUS == 42`; a child killed with SIGKILL
   (`stubborn`, then `kill(stubbornPid, SIGKILL)`) reaps as `WIFSIGNALED && WTERMSIG == SIGKILL`.
   Constants come from the fixture (42, SIGKILL), never from the code.
3. `ASiblingNeverHoldsAnotherChildsStdinOpen`: spawn A (`eof-exit`, stdin = pipe A), then B (`eof-exit`,
   stdin = pipe B); close the parent's write end of A only; A must reap with exit 7 within 2 s while B is
   still running.
4. `StopGroupEndsAStubbornGuardedChild`: spawn `stubborn` guarded; `stopGroup(pid, 200ms)` returns and
   the stubborn pid is gone within 1 s (the guard was SIGKILLed with its group, so nothing reaps the
   grandchild unless the GROUP got the signal).
5. `AMissingGuardIsAnErrorNotASilentUnguardedChild`: an explicit `guardExecutable` that does not exist
   returns `error == ENOENT` and pid 0. (When NO guard resolves at all, `spawn` runs the target unguarded
   but still as its own group leader, `getpgid(pid) == pid`, and reports `guarded == false`, so the
   caller can publish it; asserted in the same test with `guardExecutable = ""` and
   `COREVIDEO_CHILD_GUARD_PATH` pointing at a missing file.)

**Named mutations (each must turn the named test red):**
- M2a: spawn the target directly, never through the guard → test 1 red (stubborn survives).
- M2b: drop `FD_CLOEXEC` from the lifeline write end → test 1 red (the guard inherits its own write end).
- M2c: drop `POSIX_SPAWN_CLOEXEC_DEFAULT` (and the manual close-on-exec of mapped pipes) → test 3 red.
- M2d: guard exits 0 instead of mirroring → test 2 red.
- M2e: `stopGroup` signals `pid` instead of `-pid` → test 4 red.

---

## Task 3 (P1, native): the sender spawns through the helper; `hls` is an isolated destination

**Files:** `RtmpOutputSenderAdapter.cpp` (POSIX branch of the process start, `interrupt()`, the stop
path), `OutputDestinationSupervisor.h`, `native/tests/FfmpegChildRetirementTest.cpp` (new POSIX cases),
`native/tests/OutputDestinationSupervisorTest.cpp`.

**Behavior.** The POSIX start builds a `SpawnRequest` (video pipe → fd 0, audio pipe → fd 3, stderr →
the temp log, stdout → /dev/null) and calls `posix_child::spawn`; the pid it stores is the guard's.
`interrupt()` becomes `signalGroup(pid, SIGTERM)`; the stop path keeps its 0.5 s EOF wait and replaces
its SIGTERM/SIGKILL with `stopGroup(pid, 500 ms)`. The `ffmpeg-process-start` event gains
`"childGuard": true|false`; an unguarded start logs one `[rtmp] child guard not found` line. Exit codes
read by the existing `waitpid` code are unchanged because the guard mirrors them.
`destinationIsolationTraits("hls")` returns `{inProcess:false, interruptible:true}`; the comment says
"a job object on Windows, a process group under corevideo-child-guard on POSIX".

**Tests:**
1. `FfmpegChildRetirement.PosixSenderRunsFfmpegUnderTheGuardAndStopReapsIt` (POSIX, RTMP build): copy
   the probe as `ffmpeg` into a temp dir with mode `stubborn` selected by an env var the probe reads
   (`COREVIDEO_TEST_PROBE_MODE`, `COREVIDEO_TEST_FFMPEG_PID_FILE`), point `COREVIDEO_FFMPEG_BIN_DIR` at it,
   start an RTMP sender at `rtmp://127.0.0.1:1/live/x`, submit frames until the pidfile appears. Assert
   the fake ffmpeg's parent is NOT this test process (`proc_pidinfo` ppid) and its pgid differs from ours;
   then `stop`, and assert the fake ffmpeg pid is gone within 2 s.
2. `OutputDestinationSupervisor.HlsIsAnOutOfProcessInterruptibleDestination`: traits for `hls`.

**Named mutations:** M3a: spawn ffmpeg directly (old `posix_spawn`) → test 1 red on the parent check.
M3b: stop path `::kill(pid, SIGKILL)` instead of `stopGroup` → test 1 red (the guard dies, the
stubborn grandchild survives). M3c: remove the `hls` branch → test 2 red.

---

## Task 4 (F1, native): one POSIX ffmpeg resolver for sender and ingest

**Files:** `PosixExecutableLocator.h`, `RtmpOutputSenderAdapter.cpp` (POSIX branch only),
`SrtIngestCaptureAdapter.cpp` (POSIX branch only), `native/tests/PosixExecutableLocatorTest.cpp`.

**Interface.**
```cpp
struct FfmpegSearch {
  std::string configuredDirectory;                  // destination's ffmpegBinDirectory
  std::function<const char*(const char*)> getenv;   // injectable
  std::filesystem::path executableDirectory;        // where corevideo-native lives
  std::filesystem::path currentDirectory;
  const char* pathEnv = nullptr;
  std::vector<std::filesystem::path> fallbacks;     // Homebrew/MacPorts
};
std::string resolveFfmpeg(const FfmpegSearch&);
```
Order: configured dir, `COREVIDEO_FFMPEG_BIN_DIR`, `COREVIDEO_FFMPEG_DIR` (ingest's old name, kept as
an alias), `FFMPEG_BIN_DIR`, the executable's directory (the bundled copy), the current directory, PATH,
fallbacks. The sender's existing order is unchanged apart from the alias; ingest gains everything after
its old first step.

**Tests:** `PosixExecutableLocator.TheBundledCopyBeatsPathAndHomebrew` (fake executables in three temp
dirs: exe dir, a PATH dir, a fallback dir; the exe-dir copy must win); `…TheIngestAliasIsHonoured`
(`COREVIDEO_FFMPEG_DIR` alone resolves; `COREVIDEO_FFMPEG_BIN_DIR` beats it when both are set).
**Mutations:** M4a: move the executable directory after PATH → first test red. M4b: drop the alias →
second test red.

---

## Task 5 (F1, release): build, bundle, sign and audit the ffmpeg

**Files:** `scripts/build-macos-ffmpeg.sh`, `scripts/macos-ffmpeg.lock` (Task 1),
`scripts/make-macos-shell-bundle.sh`, `scripts/release-macos.sh`, new `scripts/tests/test-macos-ffmpeg-bundle.sh`.

**Behavior.**
- `make-macos-shell-bundle.sh` gains `COREVIDEO_FFMPEG_DIR_BUILT` (the build script's output dir) and
  copies `bin/ffmpeg` → `Contents/Resources/ffmpeg` and `notices/` → `Contents/Resources/notices/`, and
  copies `corevideo-child-guard` from the core build dir. Both are `need_helper`s (fatal under
  `COREVIDEO_REQUIRE_HELPERS=1`; dev bundles warn). Developer ID signing adds both with their identifiers
  and no entitlements (D5), before the app is sealed. Ad-hoc bundles sign them ad-hoc.
- `release-macos.sh` builds the ffmpeg (`scripts/build-macos-ffmpeg.sh`, cached), builds
  `corevideo-child-guard` with the core, passes both to the bundle script, re-runs
  `build-macos-ffmpeg.sh --audit` on the copy INSIDE the signed app, and the manifest's
  `runtimeRequirements` drops "needs Homebrew ffmpeg" and gains an `ffmpeg` block
  (version, licence, sha256 of the shipped binary, configure line source = PROVENANCE.txt).
- `test-macos-ffmpeg-bundle.sh`: assembles an ad-hoc bundle into a temp dir from the current build and
  asserts the three files exist, both executables pass `codesign --verify`, `--audit` passes on the
  bundled copy, and `PROVENANCE.txt` names the lock's three SHA-256s.

**Proof (not a unit test):** the audit refuses Homebrew's GPL ffmpeg (exit 1: GPL `-L`, `--enable-gpl`,
11+ non-system dylibs, no `srt`) and accepts the built one. A Developer ID bundle (signing only, no
notarization) is assembled in the scratch directory and `codesign -dvv` shows `runtime` + timestamp on
`ffmpeg` and `corevideo-child-guard`; the E2E in Task 9 streams through that signed copy.
**Mutations:** M5a: delete the ffmpeg copy line → the bundle test red. M5b: add `--enable-gpl` to the
configure line → the build's own audit fails (shown once on a throwaway cache key, not committed).

---

## Task 6 (D1, shell): pure destination model, validation, payload and status projection

**Files:** new `mac-shell/Sources/CoreVideoProShell/StreamDestinations.swift`, new
`mac-shell/Sources/CoreVideoProShell/ShellTestsStreaming.swift` (cases appended in `ShellTests.runAndExit`).

**Interfaces.**
```swift
struct StreamDestinationsConfig: Equatable, Codable {
    var rtmpEnabled = true; var rtmpUrl = "rtmp://a.rtmp.youtube.com/live2"
    var srtEnabled = false; var srtMode = "caller"; var srtHost = ""; var srtPort = "9000"
    var srtLatencyMs = "120"; var srtStreamId = ""; var srtKeyLength = "0"
    var hlsEnabled = false; var hlsPlaylistUrl = ""
}
struct StreamDestinationSecrets: Equatable { var rtmpKey = ""; var srtPassphrase = "" }
enum StreamDestinationPolicy {
    static func validateSrt(mode:host:port:latencyMs:streamId:keyLength:passphrase:) -> String?  // Windows ValidateSrt, verbatim text
    static func validateHls(_ playlistUrl: String) -> String?                                   // Windows ValidateHls, verbatim text
    static func validateSelected(_ c: StreamDestinationsConfig, _ s: StreamDestinationSecrets) -> String?
    static func enabledDestinations(_ c:, _ s:) -> [String]            // only VALID enabled rows, rtmp/srt/hls order
    static func destinationSettings(_ c:, _ s:, bitrateMbps: Double) -> [JSONObject]
}
struct DestinationStatus: Equatable { var label: String; var tone: Tone; var detail: String; var showsRetry: Bool; var isLive: Bool }
enum DestinationStatusProjection { static func project(_ sender: JSONObject?, desired: Bool) -> DestinationStatus }
```
Projection table (fixture-derived expectations, D9/D10):

| lifecycle.state | supervisor | label | retry | isLive |
|---|---|---|---|---|
| (no sender), desired | — | "Starting" | no | no |
| (no sender), not desired | — | "Off" | no | no |
| requested / preparing, `consecutiveFailures == 0` | healthy=false | "Connecting" | no | no |
| preparing, failures ≥ 1, `nextAttemptInMs` 4200 | healthy=false | "Reconnecting in 5 s" | no | no |
| producing | healthy=true | "Live" | no | yes |
| producing | healthy=false | "Not confirmed" (no media accepted yet) | no | no |
| interrupted | any | "Interrupted" + "no media accepted for N s" from `lastProgressAgeMs` | no | no |
| any | gaveUp=true | "Stopped retrying" + supervisor `reason` | yes | no |
| failed (no give-up) | any | "Failed" + `lifecycle.error` | no | no |
| stopping / finalizing | any | "Stopping" | no | no |
| completed | any | "Stopped" | no | no |
| producing, **status "live"**, no supervisor node | — | "Unsupervised" | no | no |

The SRT payload mirrors Windows (`id:"srt"`, `mode`, `host`, `port` int, `latencyMs`, `latencyUs`,
`passphrase`, `keyLength` int, `streamId` only when non-empty, `fps`, `targetBitrateMbps`, `videoCodec`,
`encoderMode`); HLS mirrors Windows (`id:"hls"`, `protocol:"hls"`, `url`, plus the RTMP row's encoder
fields); RTMP keeps the current Mac fields.

**Tests:** the Windows validation table (`StudioStreamOutputValidationTests` HLS and SRT cases, same
inputs, same sentences); the projection table above row by row; a test that a sender whose published
`status` is `"live"` while `lifecycle.state == "preparing"` projects "Connecting", never "Live"; payload
field-by-field for each protocol, including that an invalid enabled SRT row is NOT in
`enabledDestinations` nor in `destinationSettings`; secrets never appear in any field other than
`streamKey`/`passphrase`.
**Mutations:** M6a: label from `status == "live"` → the "preparing but live" test red. M6b: drop the
`supervisor.healthy` condition → the "producing, healthy=false" row red. M6c: `showsRetry` on
`lifecycle.state == "failed"` instead of `gaveUp` → the gave-up row and the failed row both red.
M6d: include invalid enabled rows → the invalid-SRT test red.

---

## Task 7 (D1, shell): AppModel wiring, persistence, Keychain, Retry

**Files:** `AppModel.swift`, `ShellPrefs.swift`, `StreamKeychain.swift`, `ShellTestsStreaming.swift`.

**Behavior.** `AppModel.streamDestinations: StreamDestinationsConfig` (persisted in `ShellPrefs`;
`streamUrl` stays the RTMP URL and maps onto `rtmpUrl`), `srtPassphrase` (Keychain account
`srt-passphrase`, loaded at launch beside the RTMP key). `startProgramOutput` sends
`enabledDestinations` and `destinationSettings`; `toggleStreaming` refuses with
`validateSelected`'s sentence instead of "URL and key are required". Every snapshot projects
`destinationRows: [String: DestinationStatus]` for rtmp/srt/hls from `outputSenderSession.senders[]`;
`streamStatus` (read by the quit guard and the control surface) becomes the published status of the
first sender, in rtmp/srt/hls order, whose lifecycle is not terminal, so a live SRT-only stream still
blocks a careless quit. `retryDestination(_ id:)` sends one `media-core-sync` with
`[{"type":"recover-output-sender","destination":id,"reason":"operator-retry"}]`.

**Tests:** with the existing command-capture seam: enabling SRT + RTMP sends `destinations:["rtmp","srt"]`
and two settings rows; an invalid SRT row alone refuses to start with the Windows sentence and sends
nothing; `retryDestination("srt")` sends exactly the recover command; a snapshot with RTMP producing+
healthy and SRT gave-up yields rows Live / Stopped retrying (retry) and `streamStatus` from the
RTMP row; prefs round-trip keeps every new field and never contains the passphrase.
**Mutations:** M7a: send every enabled row regardless of validity → refusal test red. M7b: Retry sends
`stop-encoder-session` → Retry test red. M7c: persist the passphrase in prefs → the prefs test red.

---

## Task 8 (D1, shell UI): the destinations list

**Files:** `StudioFooter.swift` (stream flyout), `SettingsPane.swift` (a "Streaming destinations" card),
new view `DestinationRowsView` in `StreamDestinations.swift`.

**Behavior.** The flyout lists the three rows: toggle, name, status label (tone colour), detail line,
Retry button when `showsRetry`. RTMP URL/key fields stay in the flyout; SRT and HLS fields live in the
Settings card (mode picker, host, port, latency, stream id, key length picker, passphrase SecureField;
HLS playlist URL), each with the validation sentence under the row. "SRT and HLS destinations are
Windows-only for now." is removed. The footer help/status line reads the projection ("Streaming — RTMP
live, SRT reconnecting"), never the raw status. The settings card shows the ffmpeg the core resolved
(`runtimeDetail`) as the Health readout F1 asks for. `scripts/lint-mac-design.sh` stays green.

**Proof:** design lint; a `COREVIDEO_SHELL_TAB_SNAPSHOT` PNG of Settings if the harness can render it
here (recorded either way).

---

## Task 9: end-to-end proof and outcomes

1. Assemble a Developer-ID-signed bundle (signing only) into the scratch directory; confirm `ffmpeg` and
   `corevideo-child-guard` carry runtime + timestamp.
2. Start a local receiver with Homebrew ffmpeg (`-listen 1` RTMP on 127.0.0.1, and an SRT listener),
   NEVER a real platform.
3. Run the signed bundle's `corevideo-native` headless over stdio with `PATH=/usr/bin:/bin` (note: the
   resolver's Homebrew fallback would still find `/opt/homebrew/bin/ffmpeg`, so the proof is the
   `ffmpeg-process-start` event's `ffmpegExecutable` and `ps` showing the bundled path), start the
   fake engine's synthetic sources onto Program, stream for 20 s to RTMP and SRT.
4. Decode what the receiver wrote: ffprobe stream info, and mean luma over decoded frames must not be
   flat (a range across frames, and not ≈16 or ≈0 everywhere).
5. While streaming, SIGKILL the core; within 5 s `pgrep -f Resources/ffmpeg` and the guard pids must be
   empty.
6. Write `docs/superpowers/plans/2026-11-21-mac-streaming-phase2-outcomes.md`.
