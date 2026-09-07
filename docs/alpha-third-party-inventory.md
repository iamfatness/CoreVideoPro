# Alpha third-party packaging inputs

Audit date: 2026-09-07. This is an engineering inventory, not a determination that
all redistribution obligations are satisfied. Reconcile it against the final
package's actual file hashes and resolved dependency versions before shipping.
The files under `alpha-third-party-inputs/` preserve inspected upstream texts and
local FFmpeg version/build configuration; they are inputs to the package notices.

## Alpha download strategy

The public alpha ZIP must omit the unproven local FFmpeg binaries. The first-launch
helper `scripts/alpha/Install-MediaRuntime.ps1` instead downloads this pinned
[upstream LGPL shared x64 archive](https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-05-31-13-22/ffmpeg-N-124714-g49a77d37be-win64-lgpl-shared.zip):

- Size: 88,569,449 bytes.
- SHA-256: `56f4a1d367e9537f63849e5cf9103824f6d87f4fc39a6a22b717b4df186da054`.
- Revision: `49a77d37be`; this is distinct from the earlier local May 19 runtime.
- Downloaded archive hash was verified against the upstream release asset digest.
- Required ABI files confirmed in the archive: `avcodec-62.dll`, `avformat-62.dll`,
  `avutil-60.dll`, `swscale-9.dll`, `swresample-6.dll`.

Only root `bin/*.dll`, `bin/ffmpeg.exe`, and root license/notice/readme material are
eligible for installation. The archive's LICENSE is installed under
`notices/ffmpeg/`, together with download provenance written after installation.
No system PATH, registry, account, or installed application is changed. Runtime
compatibility still requires testing; ABI matching alone is not a feature or
performance acceptance result. The earlier local-source investigation below is
retained as provenance history, not as the source of the alpha's downloaded DLLs.

| Component | Observed version/input | Packaging action |
| --- | --- | --- |
| FFmpeg shared Windows runtime | `N-124549-g1572784128-20260519`; `avcodec-62`, `avformat-62`, `avutil-60`, `avdevice-62`, `avfilter-11`, `swresample-6`, `swscale-9` | Include inspected LGPLv3 text, version/build configuration, and verified corresponding source/build/dependency information. See unresolved provenance below. |
| Zoom Meeting SDK | Local packaging record says Windows x64 `7.0.5.39292`; verify final SDK binary versions | Retain the SDK runtime's `directui_license.txt`, `duilib_license.txt`, and `nanosvg_LICENSE.txt`. These component texts do not replace the SDK distribution terms or prove permission to redistribute the full SDK. |
| .NET runtime | Local self-contained runtime package `9.0.16` | Include its `LICENSE.TXT` and `THIRD-PARTY-NOTICES.TXT`; use the actual selected runtime version if publish resolves another. |
| Windows App SDK runtime / WinUI | NuGet `2.2.0` / `2.2.1` | Include each package's `license.txt` and `NOTICE.txt`; verify self-contained publication includes its required runtime files. |
| CommunityToolkit.Mvvm | NuGet `8.4.0` | Include `License.md` and `ThirdPartyNotices.txt`. |
| Other managed/native dependencies | Vortice, SharpGen, WinRT, WebView2 loader, Microsoft SDK packages and transitive dependencies in final `.deps.json` / `project.assets.json` | Collect the resolved package license expressions/files and notices; the captured subset is not a complete transitive inventory. |
| Bundled fonts | Space Grotesk and IBM Plex Mono | Captured `SpaceGrotesk-OFL.txt` and `IBMPlexMono-OFL.txt` from the Google Fonts upstream repository; include them with the final Assets folder and retain attribution. |

Font notice sources: [Space Grotesk OFL](https://github.com/google/fonts/blob/main/ofl/spacegrotesk/OFL.txt)
and [IBM Plex Mono OFL](https://github.com/google/fonts/blob/main/ofl/ibmplexmono/OFL.txt), retrieved 2026-09-07.

## FFmpeg provenance

The local build reports shared libraries, `--enable-version3`, and disabled x264
and x265. Its complete configuration is captured beside this document. The
FFmpeg revision resolves to the project's own
[commit 1572784128](https://github.com/FFmpeg/FFmpeg/commit/1572784128).
This identifies FFmpeg source, **not** the full corresponding source for all
enabled external libraries, patches, or the binary build recipe.

The configuration resembles [BtbN's build system](https://github.com/BtbN/FFmpeg-Builds),
but no original archive URL, checksum manifest, or exact build-system revision was
recovered locally. Do not substitute a guessed nightly download link as verified
provenance. Obtain the original matching binary/source bundle or rebuild from a
pinned, recorded toolchain and dependency set. The FFmpeg project provides its
[distribution checklist](https://ffmpeg.org/legal.html); use the applicable license
texts and actual build contents when completing the notices/source package.

Follow-up provenance checks: the repository Actions variable
`COREVIDEO_FFMPEG_URL` was absent, the local Downloads folder had no FFmpeg archive,
and the installed executable had no retained download-origin stream. The complete
BtbN release listing retained May 31, not May 19. The last build-system commit
before May 19 at 13:00 UTC is
[`7b5dc8c88b43af71a7284f7f8919d1e6b004ebc1`](https://github.com/BtbN/FFmpeg-Builds/commit/7b5dc8c88b43af71a7284f7f8919d1e6b004ebc1);
this is a candidate recipe for investigation, not proof it produced the local DLLs.
The retained May 31 binary is revision `49a77d37be`, which does not match the local
revision. Its release assets did not include a separately named source bundle.

## OAuth eligibility and package hygiene

The production manifest configures `https://corevideo.iamfatness.us/oauth/start`,
its `/oauth/callback`, and `corevideo://oauth/callback`. Source code uses the broker
for authorization and SDK JWT retrieval. A public URL and public client ID do not
prove Zoom Marketplace publication or eligibility of unrelated friend accounts.
No account-side publication or tester allowlist information was available in the
inspected repository. The release owner must confirm eligibility; no account
changes or user sign-in were performed for this audit.

Build the distributable from a clean, explicit file manifest. Exclude Recordings,
logs, personal preferences, OAuth stores, PDBs, support bundles, test/fake engines,
and local staging diagnostics. Never copy `%LOCALAPPDATA%\CoreVideoPro` into it.
The earlier offline candidate contained recordings and a fake Zoom engine and
must not be used as an unrestricted directory-copy source. Include the tester
guide, notices, version, SHA-256 inventory, and known limitations in the clean ZIP.
