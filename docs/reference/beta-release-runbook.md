# Cutting a Windows beta (local, unsigned) — the runbook

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Betas are built LOCALLY and published as GitHub PRE-releases tagged `beta-YYYY-MM-DD-<sha7>`.
The tag-driven `release.yml` (`v*`) needs signing secrets that do not exist yet, so it is not the
beta path. Steps:

1. **Worktree.** Make a fresh DETACHED worktree at the main commit, then copy
   `native-core/zoom-runtime/windows` in.
2. **Native core.** `npm run build:native-dev` with `ZOOM_SDK_DIR` set. Never hand-write cmake:
   that once silently built a software "Stub" core. Confirm `COREVIDEO_WITH_D3D11:BOOL=ON`. Run
   `native/build-dev/corevideo-native-tests.exe`.
3. **Shell.** Publish from a CLEAN tree:
   `dotnet publish native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -r win-x64
   --self-contained true -p:WindowsAppSDKSelfContained=true -p:Platform=x64 -o <publish>`.
4. **Package and install.**
   - `scripts/package-alpha.ps1 -ReleaseId … -PublishDirectory … -NativeBuildDirectory …`
   - `scripts/package-alpha-installer.ps1 -Archive <zip> -VcRedist <VS-bundled vc_redist.x64.exe>`.
     The copy in Downloads is too old.
   - `scripts/alpha/Test-AlphaPackage.ps1` and `Test-AlphaInstaller.ps1`, which does a real silent
     install, runs the startup probe, then uninstalls.
   Both scripts stage under short user-owned roots so a long checkout path cannot break NSIS:
   `-StagingRoot` (default `%LOCALAPPDATA%\Temp\cvp-pkg\<8 hex>`) for packaging and `-WorkRoot`
   (default `%LOCALAPPDATA%\Temp\cvp-qa\<8 hex>`) for the installer test. `artifacts/installer-build/<id>/`
   now holds only `makensis.log` and the generated `.nsh` files; staging is deleted on success.
   On a workstation with existing CoreVideo shortcuts, this test deliberately
   refuses to overwrite them. Upload the six assets to a draft release and run
   the `Beta package qualification` workflow with its tag and full source SHA.
   The clean Windows runner verifies tag/manifest/hash identity, exercises the
   real package validator's negative fixtures, installs, probes and uninstalls
   those exact assets. Publish only after that job and main CI pass.
   Silent setup requires an administrator context and the manifest's VC runtime
   minimum already installed. The workflow verifies and installs the exact
   Microsoft-signed redistributable embedded in the installer before exercising
   failure cases; a prerequisite rejection must not count as an ownership test.
5. **Publish.** `gh release create <tag> --prerelease --target <sha>` with the six assets: the zip,
   the Setup .exe, their `.sha256` files, their manifests, and the top-level manifest.

**THE STALE-PRI TRAP (caught before publish on beta-2026-09-10-3a3bedf).** A publish run WITHOUT
`-p:WindowsAppSDKSelfContained=true` leaves an app-only `CoreVideoPro.WinUI.pri` (~177 KB), and a
later flagged publish REUSES it. The installed app then dies at launch with `XamlParseException:
Cannot locate resource from 'ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml'`. The file
count is identical, so only these two gates catch it, and both must pass before packaging:
- the `.pri` is ~2.36 MB;
- `CoreVideoPro.WinUI.exe --verify-runtime <json>` on the publish exits 0.

**Before and after, live.** `python scripts/qa/live-check-sources-audio.py [seconds]` reads ONLY the
control API, sends no input, and checks four things in a real meeting:
- wall guests have video;
- camera-off participants hold no video feed;
- the app muted no guest;
- talking causes no subscription churn, meters are live, and the snapshot is fresh.

Run it on the previous build first, then on the new one.

## Stable releases (`vX.Y.Z`, first cut v1.0.0 on 2026-11-11)

A stable release uses this same local, unsigned path with a semantic-version ID instead of a
dated beta ID. Owner ruling 2026-11-11: ship 1.0 unsigned now and sign in a later patch once a
certificate exists.

- Bump first: `npm run bump:version -- <major|minor|patch>` sets `package.json`, the csproj
  `ApplicationDisplayVersion` and the appxmanifest identity (the shell's file and product
  version), and rolls `CHANGELOG.md`. Merge that to main, then build and package from the merged
  commit so `sourceCommit`, `applicationCommit` and `packagingCommit` all name the tag commit.
- `scripts/package-alpha.ps1 -ReleaseId v1.0.0 …`: the ID `vX.Y.Z` selects the **release**
  channel. The payload root is `CoreVideoPro-Release`, the packaged README is
  `docs/release-guide.md`, the installer's product name is plain "CoreVideo Pro" with file
  version `X.Y.Z.0`, and its pages drop the "rehearsals and testing" wording while still saying
  the app is not code-signed. Every script that validates a release ID (packaging, installer
  test, camera registration, previous-install retirement) accepts both forms, so installing a
  release retires beta installs and a later beta retires the release.
- `Beta package qualification` accepts a `vX.Y.Z` tag; publish with `gh release create <tag>
  --latest --target <sha>` (not `--prerelease`).
- **Pushing a `v*` tag also starts `release.yml`**, the signed MSIX pipeline. Without signing
  secrets and `ZOOM_SDK_URL` it fails by design, never silently. Cancel that run and say why in
  the release notes until signing exists.
