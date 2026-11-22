# macOS dependable streaming, Phase 2 (F1, P1, D1): outcomes

Branch `mac-streaming-phase2` (worktree `.claude/worktrees/mac-streaming-phase2`), cut from `fc675541`
(Mac Phase 1, PR #1220). Plan: `2026-11-21-mac-streaming-phase2.md`. Not pushed, no PR.

## Commits

| Commit | Task | What |
|---|---|---|
| `85bc6a13` | 1 | Plan, `scripts/macos-ffmpeg.lock`, `scripts/build-macos-ffmpeg.sh` |
| `2c76cd7a` | 2 (P1) | `posix_child` spawn helper + `corevideo-child-guard` + probe + 5 tests |
| `42f7f642` | 4 (F1) | One POSIX ffmpeg resolver for sender and ingest (bundled copy beats PATH/Homebrew) |
| `17d427ce` | 3 (P1) | RTMP/SRT/HLS ffmpeg runs under the guard; `hls` isolation trait |
| `535b7c7f` | 5 (F1) | Bundle, sign and audit ffmpeg + guard; release-script changes; bundle test |
| `a09d2b9c` | 9 (F1+P1) | `scripts/qa/mac-stream-e2e.py`; audit refuses static Homebrew leaks |
| `85de3827` | 6 (D1) | Swift destination model, Windows validation ports, payload, status projection |
| `88f6b40d` | 7 (D1) | AppModel wiring, Keychain passphrase, Retry, prefs |
| `8ec362c8` | 8 (D1) | Destinations list UI, Settings card, footer reads the projection |

(Tasks were executed 1, 2, 4, 3, 5, 9, 6, 7, 8: the resolver was independent of the sender change,
and the end-to-end proof was run as soon as the native half was complete.)

## Test counts (exact commands)

- **Native**, CI `native-metal-macos` flags, `native/build-metal`:
  `./native/build-metal/corevideo-native-tests` → baseline **2101 passed, 0 failed**; final
  **2110 passed, 0 failed** (baseline + the 9 new tests, fresh build of the final tree). New: `PosixChildProcess.*` 5, `PosixExecutableLocator.*` +2
  (10 total), `FfmpegChildRetirement.PosixSenderRunsFfmpegUnderTheGuardAndStopReapsIt`,
  `OutputDestinationSupervisor.HlsIsAnOutOfProcessInterruptibleDestination`. Touched suites rerun:
  `OutputDestinationSupervisor*` 26/26, `OutputSenderAdapter.*` 12/12, `RtmpOutputSender.*` 2/2,
  `ContractParity.*` 13/13.
- **Shell**: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`
  → baseline **5676 checks, 0 failed**; final **5825 checks, 0 failed** (13 new `streaming/*` cases).
  One run in the middle showed `ControlApiTests.swift:936` ("a CORS preflight is not a route") failing once
  under load; it passed on the two immediate reruns and every run since. Treated as load flake, recorded here.
- `bash scripts/lint-mac-design.sh` → green.
- `scripts/tests/test-macos-ffmpeg-bundle.sh` → 16/16 PASS (ad-hoc bundle into a temp dir).
- `python3 scripts/qa/mac-stream-e2e.py --app <bundle> --srt-receiver /opt/homebrew/bin/srt-live-transmit` → PASS.

### Mutations (plan-authoring rule 7)

Every named mutation was applied alone, rebuilt, and turned its test red:
M2a no guard, M2b lifeline write end inheritable, M2c no `POSIX_SPAWN_CLOEXEC_DEFAULT`, M2d guard exits 0,
M2e pid-only stop; M3a unguarded sender, M3b pid-only SIGKILL on stop, M3c no `hls` trait; M4a executable
directory dropped, M4b alias dropped; M5a ffmpeg copy line removed; M6a label from `status=="live"`, M6b
`healthy` dropped, M6c Retry keyed on `failed`, M6d invalid rows sent; M7a refusal guard removed, M7b Retry
sends `stop-encoder-session`, M7c passphrase in prefs; M8a footer health from `streamStatus`.
**M5b** (`--enable-gpl`): ffmpeg's own configure refuses it (`OpenSSL >=3.0.0 requires --enable-version3`);
the audit's refusal of a GPL binary is shown on Homebrew's ffmpeg instead (exit 1: GPL `-L`, `--enable-gpl`,
non-system dylibs, no `srt`, embedded Homebrew paths, wrong OpenSSL).

Two tests were weaker than their names on first writing, and both were fixed:
- **M2b stayed green** at first: on Apple, `CLOEXEC_DEFAULT` already keeps the lifeline out of the guard, so
  the flag was untested. Its real job is to protect against the core's OTHER children, which start by plain
  `fork`/`exec` (Zoom engine, plugin host, browser host); one of them holding the write end would keep
  every guard alive after the core dies. The stand-in core now also starts a plain-spawned sibling, and M2b
  goes red.
- **M6c reddened only the "failed" row**, because the gave-up fixture also had lifecycle `failed`. A
  gave-up-while-`interrupted` fixture now pins give-up independently.

Process trap worth keeping: my first mutation script restored files with `mv`, which keeps the backup's OLD
mtime, so ninja kept the MUTATED binary and a clean run looked broken. Restore with `touch` afterwards.

## Proven

- **F1, bundled ffmpeg streams without Homebrew.** A Developer ID-signed bundle (hardened runtime +
  timestamp on `ffmpeg`, `corevideo-child-guard` and the core, `codesign --verify --deep --strict` OK) ran
  headless with `PATH=/usr/bin:/bin:/usr/sbin:/sbin`. Both senders resolved
  `…/CoreVideo Pro.app/Contents/Resources/ffmpeg` (`runtimeDetail`) and reached `lifecycle=producing`,
  `supervisor.healthy=true`. Local receivers only: Homebrew ffmpeg `-listen` for RTMP, and libsrt's
  `srt-live-transmit` for SRT. Both received **1920x1080 H.264 High 30 fps + AAC**, about 611-618 decoded
  frames each, **mean luma 148.2, range 139.0..157.5** (a moving picture, not black). Artifacts are in the
  scratchpad `e2e-final/`.
- **P1, SIGKILL leaves nothing.** In the same run each ffmpeg was the child of a guard, in its own process
  group, and each guard was the core's child. After `kill -9` of the core, no bundled ffmpeg, guard or (fake)
  engine remained **1.27 s** later. The unit tests prove it for a child that ignores SIGTERM (2 s grace,
  then SIGKILL).
- **Licence / packaging.** `ffmpeg -L` reports **LGPL version 3 or later**; buildconf has no gpl/nonfree;
  `otool -L` on the bundled ffmpeg and guard shows only `/usr/lib` and `/System/Library`; no `/opt/homebrew`
  or `/usr/local` string in either; the statically linked OpenSSL is **3.5.9** (the pin), not Homebrew's
  3.6.3. The core's single `/opt/homebrew` string is the resolver's fallback-directory literal (a search
  path, not a load reference).
- **D1.** The status labels come from lifecycle + supervisor only; a sender whose published status is
  `live` while its lifecycle is `preparing` reads "Connecting" (test). Retry sends `recover-output-sender`.
  Settings tab renders offscreen (`COREVIDEO_SHELL_TAB_SNAPSHOT`); PNG in the scratchpad `tabs/`.

## Unverified (honestly)

- **Notarization.** Not submitted (the orchestrator builds installers). The new Mach-Os carry no
  entitlements and link only the OS, so nothing obvious should fail, but it is unproven until
  `release-macos.sh` runs.
- **Clean-Mac install.** Proven on this Mac with Homebrew present but not on PATH (the resolver prefers the
  bundle, shown by path). Not proven on a machine with no Homebrew at all.
- **HLS end to end.** The validation, payload and isolation trait are tested; no HLS PUT receiver was
  stood up.
- **RTMPS** (TLS through the static OpenSSL) was not exercised: no local TLS RTMP server.
- **The fault drills in spec §7:** receiver killed mid-stream (`interrupted` then recovery), DNS failure,
  and a live give-up exercising the Retry button against a real supervisor. These are covered by the
  projection tests and by the existing `OutputDestinationSupervisorTest`, not by a live run.
- **Shell end to end.** The UI was not driven against a live core: wiring is proven by the
  command-capture and snapshot tests and the offscreen render.
- **Other helper children.** The real Zoom engine, plugin host and browser host do not use the guard.
  The fake engine exited on core death in the E2E; the real ones were not tested here.
- **Linux.** There is no `CLOEXEC_DEFAULT` there; the sender marks its own pipes close-on-exec, but the
  stub/TSan Linux builds were not compiled from this machine (the new code is POSIX and Apple-guarded
  where it differs).

## Defaults taken (full table in the plan)

- **ffmpeg sourcing:** a pinned **source** build, not a prebuilt. ffmpeg 8.1.3 + OpenSSL 3.5.9 (LTS) +
  libsrt 1.5.7, by URL + SHA-256 in `scripts/macos-ffmpeg.lock` (OpenSSL's hash cross-checked against
  upstream's `.sha256`; ffmpeg and libsrt are trust-on-first-use). It is built by
  `scripts/build-macos-ffmpeg.sh` into `~/Library/Caches/CoreVideoPro/ffmpeg-lgpl/<sha16(lock+script)>`:
  about 80 s on an M3 Max, cached afterwards. It needs only Xcode CLT, cmake, perl, curl. It uses
  `--disable-autodetect` and a pkg-config shim that knows only the two in-tree deps, so no Homebrew library
  can leak in (audited, see above).
- **Licence:** **LGPL v3-or-later** (`--enable-version3`), one static executable run as a separate
  process. This was changed during Task 5: ffmpeg's configure only forces version3 for OpenSSL 3 when GPL
  is on, so the first build claimed "LGPL 2.1+" around statically linked Apache-2.0 code. Notices ship at
  `Contents/Resources/notices/ffmpeg/` (ffmpeg LICENSE.md + COPYING.LGPLv2.1/v3, OpenSSL and libsrt
  licences, PROVENANCE.txt with versions, URLs, SHA-256s and exact configure lines). **No source tarballs
  in the pkg.**
- **Bundle size added:** `ffmpeg` **26 MB**, `corevideo-child-guard` 56 KB, notices 80 KB: **about 26 MB**
  uncompressed.
- **Child lifetime:** a separate `corevideo-child-guard` executable (not a core re-exec mode); 2 s
  SIGTERM→SIGKILL grace on lifeline EOF; group signals for interrupt and stop.
- **D1:** three fixed rows (one sender per protocol id); Live = producing AND supervisor healthy; Retry
  only on `supervisor.gaveUp`; SRT passphrase in the Keychain (account `srt-passphrase`); rows lock while
  streaming; stream recipe unchanged (O1 owns it); remote-control `transport.stream.rtmp.configure` and
  the blocked `transport.stream.srt.configure` are untouched.

## Release-script changes the orchestrator must know about

- `release-macos.sh` now **builds the ffmpeg** (`scripts/build-macos-ffmpeg.sh`, cached; the first run on
  a machine downloads about 66 MB of pinned sources and builds for about 80 s), builds
  `corevideo-child-guard` with the core, passes `COREVIDEO_FFMPEG_BUILT_DIR` to the bundle script, and
  **fails the release** if the signed copy inside the app fails the LGPL/capability/static-leak audit,
  carries any entitlement, or is not signed as `us.iamfatness.corevideopro.ffmpeg` /
  `us.iamfatness.corevideopro.child-guard`. New preflight tools: `perl curl make`.
- `make-macos-shell-bundle.sh`: two new **required helpers** under `COREVIDEO_REQUIRE_HELPERS=1`
  (`Contents/Resources/ffmpeg`, `Contents/Resources/corevideo-child-guard`) plus
  `Contents/Resources/notices/ffmpeg/`. Dev bundles without `COREVIDEO_FFMPEG_BUILT_DIR` warn and skip
  ffmpeg, as for any helper.
- `manifest.json`: `runtimeRequirements` drops "needs Homebrew ffmpeg"; a new `ffmpeg` block records
  version line, licence, sha256 of the shipped binary, notices path and the guard; `bundledHelpers`
  includes `ffmpeg`.
- Mach-O audit: both new binaries go through the existing "Developer ID + runtime + timestamp, arm64"
  loop unchanged.
- Tools the orchestrator installed (nasm, pkgconf, autoconf, automake, meson, srt): the ffmpeg build uses
  **none** of them on purpose. `srt-live-transmit` was used only as the E2E SRT receiver. Nothing else was
  installed.

## Spec and plan corrections found while executing

1. The `ffmpeg-process-start` event goes to the sender's send-proof file, not stdout; the snapshot's
   `runtimeDetail` (`ffmpeg:<path>`, later `available:<path>`) is the observable resolved path.
2. The footer's Stream stat and the stream button's help printed the published `streamStatus` verbatim,
   another "live from a launch" read. Both now use the projection (`streamHealthLabel`/`streamSummary`).
3. Three existing shell fixtures gave the RTMP row no key; only valid rows are sent now (Windows rule), so
   they now set one.
4. The pre-existing POSIX descriptor inheritance (spec correction 5 in the plan) was also observed live:
   the naive red run left two orphaned probe children holding each other's stdin open.

## Owner follow-ups

- **Written offer of source** for the LGPL binary needs a contact address (or ship the three pinned
  tarballs beside the pkg). Not invented here.
- Move the ffmpeg build into CI (the spec's recommendation) once the release path is proven.
- Decide whether the Zoom engine, plugin host and browser host should also start through the guard (they
  rely on their own stdin EOF today; B1 covers the browser host).
