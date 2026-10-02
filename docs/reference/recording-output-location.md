# Recordings land somewhere findable (#469 / T2.8, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`RecordingFolderPolicy.Resolve` (MediaCore, pure) decides the recording folder,
and it is the ONLY rule: the shell's default and the prefs restore both go
through it.

**The issue's premise was wrong and the fix is not what it asked for.** The
DEFAULT has been an absolute `Videos\CoreVideo Pro` since 2026-07-21. What
actually bit the owner is a **persisted RELATIVE preference**, which bypasses the
default entirely and is resolved by the CORE against its own working directory —
the install folder for an installed build, where a version-isolated upgrade or an
uninstall can strand or delete a show. Verified live 2026-09-12: the stored value
was the literal `Recordings/CoreVideo Pro`.

Rules: an absolute path is returned UNTOUCHED (it migrates accidents, never
decisions, and that includes a UNC share); the one legacy value maps onto the
user folder itself rather than nesting under it; any other relative path keeps
its shape under the user folder; a drive-rooted path stays on its drive; and with
no Videos folder the result is still ABSOLUTE (LocalApplicationData) — the old
fallback there was the relative wire default, i.e. the bug. The record flyout
shows the resolved path, because "operators can't find their files" is half the
issue. Tests: `RecordingFolderPolicyTests`.
