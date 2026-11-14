# CoreVideo Pro for macOS 1.0 — release outcomes (2026-11-14)

Plan: `2026-11-14-mac-release-1.0.md` (8 tasks, Task 8 added on owner report). All tasks
complete with independent task review + fix rounds, a whole-branch review and one fix wave
whose re-review was clean. Code commits `5a332ee9..611f89b8` on `mac-parity-1.0`.

## The release candidate

- `dist/macos/1.0.0/CoreVideoPro-1.0.0-arm64.dmg` (not committed), sha256
  `859defc4e13e92a12d7b2d19cc04cc785e3664fed64b63f9e63a8e9f27632b5b`, built from
  `611f89b8`, version 1.0.0 build 2862.
- **Notarized and stapled** (keychain profile `CoreVideo Pro`; app submission
  `ae2e54c0-23c1-477e-9245-2d44092ad4c1`, DMG submission
  `dd218908-30be-44ca-ba17-e40b7ecfe33d`). Independently verified: `spctl` → "accepted,
  source=Notarized Developer ID" for the DMG and the app inside it; `stapler validate` OK on
  both.
- Release gate in `scripts/release-macos.sh`: full native ctest incl. `realtime` label
  (100% of 5), 100 Mach-O files audited (Developer ID + hardened runtime + timestamp),
  helpers bundled (`corevideo-native`, `corevideo-plugin-host`, `corevideo-zoom-engine.app`).
- Final suite state: mac-shell harness 3104/0, selfcheck PASS, design lint green,
  Companion module vitest 17/17.

## Found along the way (beyond the plan)

- Companion couldn't load the action list from **Windows or Mac** (camelCase reader vs the
  PascalCase Windows manifest) — fixed in the module (tolerant reader); Windows users get it
  on module update.
- RTMP output did not compile on macOS (Windows-only guards); fixed with guard-only edits,
  Windows token stream unchanged. CI's mac job now builds the release config incl. RTMP.
- A Finder/Dock-launched app could not find Homebrew ffmpeg (launchd PATH) — the core's
  shared `PosixExecutableLocator` now probes Homebrew/MacPorts and the real exe dir.
- Pre-existing: reopening the main window ran `start()` again → second core. Fixed (idempotent
  `start()`, no New Window, Dock reopen reuses the window).

## Rulings made on the owner's behalf (in order, with cost if wrong)

1. Mac 1.0 scope = M0 + Take/FTB/keyboard/footer + control-API subset + Companion +
   signed/notarized DMG + CI/docs/acceptance; Graphics/Comms/social/Tiles/inspector are later
   Mac releases — if the owner expected full parity now, continue the roadmap.
2. Task order T1..T7 by dependency — rework.
3. T7 also fixes the M0 native link-gate minor and the quit-panel screenshot — none.
4. Cut/Fade picker not persisted (Windows is in-memory, default Fade) — operator re-picks
   Cut each launch.
5. Record/Stream icon-only per the Windows RC ruling; NDI badge omitted (no NDI on Mac) —
   trivial revert if the owner prefers labels.
6. T4 server-hardening minors folded into T5 — small scope growth.
7. Task 8 (top nav to Windows 1.0) added on the owner's report — none.
8. Settings stays the trailing nav item (Mac splits Zoom tab + Health pane; Settings holds
   output/record/stream config with no Windows nav slot) — larger restructure if the owner
   wants Windows' merged Zoom=Settings page.
9. Pro notary profile is `CoreVideo Pro` (owner correction) — none.

## Deferred minors (non-blocking; triaged by the final review)

- Take: `takeRevision` counts Takes only (core only echoes it).
- FTB: no visual in-flight state on the button (label/status cover it).
- Footer: `ViewThatFits` wrap layout never seen in pixels (min width 1360).
- Control API: `nodes` sorted vs Windows document order; redaction-failure `reasonCode`;
  `transport.record.toggle` can report failure after issuing a stop when intent is off
  (misleading on a Stream Deck — fix post-release); request timeout cancelled at dispatch;
  in-flight invokes not drained at stop; server stays open during the quit finish window
  (Record/Stream refused there; Take/FTB allowed = Windows `CanTake`).
- Companion: `pick()` prefers the camelCase key even when null; unknown param types become
  string.
- Packaging: `cafebabe` magic also matches Java class files (fails closed); hard-coded
  identity / Zoom SDK defaults (env-overridable); cross-filesystem partial `mv` edge in the
  publish step.
- Acceptance §0 says a *skipped* realtime test blocks the release, but early-return skips
  count as passes (unreachable with AVF on) — fix wording or grep the skip marker.
- A rapid double Dock-reopen with only a pop-out open could open two console windows (still
  one core) — add an open-in-flight latch.

## Owner acceptance

Everything automation could not prove is in `docs/qa/macos-1.0-owner-acceptance.md`
(real Cmd-Q incl. double Cmd-Q and panel fallback, close-window → Dock reopen → one core,
real camera ISO count, Zoom sign-in on the signed build, TCC persistence, Take fade / FTB
visuals, keyboard, Companion/Stream Deck, Finder-launched RTMP, 1080p60 per-frame delivery,
full rehearsal, notarized install).
