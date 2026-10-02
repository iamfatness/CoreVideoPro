# Installer: one entry point, and uninstall removes what it caused (T2.7/T2.1, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Three rules, all learned from the owner's own machine:

- **ONE stable `CoreVideo Pro` shortcut** (desktop + Start menu), repointed at
  each install. The owner accumulated five version-named desktop shortcuts,
  launched a three-day-old build by mistake and filed a defect against code that
  already had the fix. The version-named shortcut survives in the Start menu
  ONLY. Uninstall REMOVES the stable pair rather than repointing it: choosing
  "the newest remaining release" means ranking sibling folders, and a wrong guess
  silently opens the wrong build — the exact failure this exists to stop.
- **Uninstall removes the first-run media runtime, from a MANIFEST.**
  `Install-MediaRuntime.ps1` records what it wrote to
  `notices\ffmpeg\installed-files.txt`; `un.RemoveMediaRuntime` deletes that set
  and nothing else. A manifest rather than a hard-coded list because the file set
  follows the pinned upstream build; a manifest rather than `RMDir /r` because
  `$INSTDIR` is an app folder an operator's files can land in. Measured before
  the fix: TWO uninstalled versions still holding 10 files / **195 MB each**.
  The loop refuses any manifest line that is absolute, rooted, or contains `..`
  — a file that decides what an uninstaller deletes is worth validating.
- **A refused uninstall SAYS WHY.** Refusing when the registry does not match is
  correct, but refusing silently left an operator with no Apps & Features entry
  and an uninstaller that appeared to do nothing.

**Two traps this cost, both worth remembering:**

1. **NEVER run an interactive helper from a silent installer.**
   `regsvr32` WITHOUT `/s` opens a modal result dialog, and
   `Register-VirtualCamera.cmd` deliberately omits `/s` so a tester who
   double-clicks it gets feedback. Calling that .cmd from the installer hung a
   silent install forever — two `regsvr32` processes waiting on a dialog nobody
   could see, until the 180 s `Test-AlphaInstaller` timeout. The installer calls
   `regsvr32 /s` directly. Install registers the virtual camera; **uninstall must
   unregister it**, before the payload delete, or a COM registration survives
   pointing at a deleted DLL and every app that enumerates cameras inherits a
   broken device.
2. **A UTF-8 BOM is three literal characters to NSIS.** Windows PowerShell 5.1's
   `Set-Content -Encoding UTF8` writes one, so the manifest's first path became
   `<BOM>ffmpeg.exe`, `Delete` removed nothing, and only that one file was left
   behind — a failure that looked like flaky file locking. Write manifests with
   `[IO.File]::WriteAllLines(..., New-Object Text.UTF8Encoding $false)`. Any file
   NSIS reads must be written BOM-free.

`Test-AlphaInstaller.ps1` is the gate and now asserts all of it:
`stableShortcutOnly`, `mediaRuntimeRemoved`, `manifestEscapeRefused`, alongside
the existing real silent install / runtime probe / duplicate rejection /
uninstall / user-file preservation.

**T2.1 is PARTLY done, and the remainder is blocked by Windows, not by us.** The
installer registers the virtual camera and creates the recording folder. It does
NOT enable WER LocalDumps: that key is HKLM-only. Measured 2026-09-12 — an HKCU
`LocalDumps` entry for a deliberately crashing test exe (exit `0xC0000005`)
produced **zero** dumps, while the machine's HKLM configuration produced two. A
per-user installer writing that key would ship a setting that does nothing. Full
crash dumps on a tester's machine still need one elevated step
(`scripts/setup-crash-dumps.ps1`); wiring that up is separate work.
