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
