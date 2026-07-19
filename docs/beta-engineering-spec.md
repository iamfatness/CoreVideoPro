# Beta Engineering Spec — Distribution, Supportability, Onboarding, Access

Status: written 2026-07-18, companion to `docs/beta-plan.md` (the plan: what beta
means, workstreams, cut line). This spec is the engineering design for the four
systems beta needs BUILT rather than verified: **D** distribution (B1), **S**
supportability (B4), **O** onboarding/first-run (B2), **L** access control (B3).
Grounded in a same-day audit of the packaging scripts, CI workflows, Cloudflare
services, and the shell's settings/support surface — every "exists/absent" claim
below was checked against the tree, not the older plans.

## 1. Reality baseline (audited 2026-07-18)

What exists vs. what the plans assumed:

| Area | Reality |
|---|---|
| Packaging | `scripts/package-native.ps1` (loose folder) and `package-native-msix.ps1` (unsigned MSIX with a robust MakeAppx/layout fallback chain) both stage native core + Zoom runtime + FFmpeg. Neither registers the vcam DLL, sets up crash dumps, nor creates the recording folder |
| Signing | **D2 shipped 2026-07-18:** `sign-native-msix.ps1 -Mode production` signs via Azure Trusted Signing (dlib) or PFX/thumbprint from env, requires an RFC3161 timestamp, runs `signtool verify /pa`, enforces the manifest-Publisher/cert-subject match, and **hard-fails on any gap** (distinct exit codes). `-Mode dev` (default) keeps the self-signed flow and still exits 0 without signtool, but now prints a LOUD "ARTIFACT LEFT UNSIGNED" warning. Decision logic covered by `scripts/tests/test-sign-native-msix.ps1` (10 dry-run cases). Cert identity itself still unprovisioned (D0.1) |
| Versioning | ~~THREE unsynced version sources~~ **Synced via D1 (2026-07-18):** `package.json` is the source of truth; `scripts/stamp-version.mjs` stamps `Package.appxmanifest` (`Identity Version`, still `Publisher="CN=CoreVideo Pro Dev"` — D2 owns that) and the csproj; CI `version-sync` job enforces it. Release tag validation still checks only `package.json` (fine — everything else must now match it) |
| CI release | `release.yml` builds the **loose folder** (not MSIX), never signs, never creates a GitHub Release; the artifact upload is gated on `COREVIDEO_PUBLISH == 'never'` (inverted/dead). `bump:version` + `release:notes` exist but are unwired |
| Auto-update | **Built (D4, 2026-07-18):** `scripts/make-appinstaller.mjs` emits the `.appinstaller` + `latest.json`, and the shell runs a non-blocking startup version check (`COREVIDEO_UPDATE_FEED_URL`, empty default = off). Not yet hosted — the feed URL/domain is the D0/D4 hosting decision (owner) |
| Crash handling | Shell logs unhandled exceptions to `launch.log` (and swallows recoverable COM ones); `MediaCoreSupervisor` tracks child crashes (ring of 20, respawn ≤5). **No code touches `%LOCALAPPDATA%\CrashDumps`** — no detection, no upload. `setup-crash-dumps.ps1` (elevated, HKLM) is a manual dev-rig script |
| Support bundle | REAL and tested: `SupportBundleBuilder` (MediaCore) → `%LOCALAPPDATA%\CoreVideoPro\support-bundles\*.json`, stream keys/passphrases redacted (`present-redacted`), endpoint query secrets scrubbed, covered by `SupportBundleExportTests`. Gap: JSON snapshot only — does **not** collect `launch.log` / `media-core.log` / `perf.log` / dumps, no archive, no upload |
| Secrets at rest | Zoom OAuth tokens (`zoom-oauth.json`) and RTMP stream key / SRT passphrase (`production-output-preferences.json`) are **plaintext**. `FileZoomTokenStore` has encrypt/decrypt delegates — constructed with none |
| Services | `services/licensing-api` (Stripe + KV + tier entitlements) and `services/telemetry-ingest` are deployed-able but **orphaned** — only the smoke script calls them; the shell never implements the renderer's license bridge (falls to `StubLicenseClient`). telemetry-ingest stores crashes in R2 + a KV index and requires its API key since S0 (2026-07-18). `deploy-staging-workers.ps1` deploys all three workers manually; no monitoring |
| OAuth broker | External (`corevideo.iamfatness.us`, lives in the CoreVideo repo). Shell is hard-wired to it for sign-in AND refresh (#290). Only the start URL is overridable (`COREVIDEO_ZOOM_OAUTH_BROKER_START_URL`). **Discrepancy: code default app-return URI is `corevideo://oauth/callback`; the appxmanifest + docs say `corevideopro://`** |
| First-run | No wizard, no first-launch flag. Canvas default is **already 1080p** (`MediaCoreProductionSyncContext.DefaultCanvasOutputProfile` = 1920x1080) — the plan's "4K is an RTX-4090 assumption" worry is stale; 4K is opt-in |
| Wizard targets | Monitor device, mic (deliberately default-OFF), recording folder (defaults to `%USERPROFILE%\Videos\CoreVideo Pro`), Zoom sign-in all exist as bindable settings. **Vcam enable/mirror/name are NOT persisted** (reset every launch). **No single "selected camera" setting exists** — cameras are scene sources, not a global pick |

## 2. D — Distribution

### D0 — Decisions (owner, this week; everything downstream forks on these)
1. **Signing route.** Recommend **Azure Trusted Signing** as primary (publicly
   trusted, signtool-integrated, days not weeks, no cert file to protect in CI)
   with a classic OV Authenticode purchase started in parallel as fallback —
   Trusted Signing still requires org identity validation, so start BOTH now.
2. **Package format: signed MSIX + App Installer.** `package-native-msix.ps1`
   already produces the MSIX and the manifest already declares `runFullTrust` +
   the OAuth protocol. App Installer (`.appinstaller` at a stable HTTPS URL)
   gives install + **auto-update on launch** with near-zero app code — it
   collapses the B1 "auto-update channel" item into hosting a file. Fallback if
   MSIX fights us on a reference config: signed EXE installer (Inno/WiX) + the
   D4 in-app version check.
3. **Zoom SDK redistribution** (`docs/zoom-windows-sdk-packaging.md`, still
   unresolved): ask Zoom now. Fork: (a) licensed to bundle → runtime rides the
   MSIX payload (already implemented — `Assert-MsixPayloadReady` requires
   `sdk.dll`); (b) not licensed → first-run SDK fetch: move the
   `stage-zoom-sdk.ps1` staging logic in-app, download from Zoom's marketplace
   at first launch, stage to app-data, extend `MediaCorePaths` runtime discovery
   to look there. (b) is real work — get the answer early.

### D1 — One version, stamped everywhere
`package.json` becomes the single source of truth. `bump-version.mjs` (or a new
`scripts/stamp-version.mjs` invoked by both packagers) rewrites the appxmanifest
`Identity Version` (x.y.z.0) and csproj `ApplicationDisplayVersion`. CI gains a
version-sync check; the release tag validator keeps validating `package.json`.

**Status: SHIPPED 2026-07-18.** `scripts/stamp-version.mjs` (plain Node, no
deps, targeted string replacement — no XML reformat) rewrites the appxmanifest
`Identity Version` (x.y.z.0) and the csproj `ApplicationDisplayVersion` (x.y.z)
+ `ApplicationVersion` (major\*10000 + minor\*100 + patch); `--check` exits
non-zero with a diff-style report. Invoked by both packagers before staging, by
`npm run bump:version` after the bump (a bump can never leave sources
diverged), and by the new `version-sync` job in `ci.yml`. npm entry points:
`stamp:version` / `stamp:version:check`. `release.yml` picks up the same check
as part of the D5 rework (not done here).

### D2 — Production signing mode
**Status: IMPLEMENTED 2026-07-18** (script + tests; the signing *identity* is
still pending D0.1, so production mode has been exercised via `-DryRun` only).
`sign-native-msix.ps1` grows `-Mode production`: signs via Trusted Signing
(dlib) or a PFX/thumbprint from env/secret, **hard-fails** when the toolchain or
cert is unavailable (the current exit-0 stub is fine for dev mode only — a
production pipeline must never emit an unsigned artifact silently). Timestamp
server required. The appxmanifest `Publisher` must be updated to match the real
cert subject (it is `CN=CoreVideo Pro Dev` today — MSIX install fails on
mismatch; this is part of D2, not an afterthought).

Implementation contract (D5 consumes this; full docs in the script header):
- Route auto-picked from env — exactly ONE of:
  - Trusted Signing: `COREVIDEO_SIGN_DLIB` (Azure.CodeSigning.Dlib.dll) +
    `COREVIDEO_SIGN_METADATA` (JSON: Endpoint / CodeSigningAccountName /
    CertificateProfileName); Azure credentials ride the standard
    `AZURE_TENANT_ID`/`AZURE_CLIENT_ID`/`AZURE_CLIENT_SECRET` env consumed by
    the dlib, not the script. Publisher check compares
    `COREVIDEO_SIGN_EXPECTED_PUBLISHER` (skip-with-loud-warning if unset).
  - PFX: `COREVIDEO_SIGN_PFX_PATH` + `COREVIDEO_SIGN_PFX_PASSWORD`.
  - Store thumbprint: `COREVIDEO_SIGN_CERT_THUMBPRINT` (CurrentUser\My or
    LocalMachine\My; machine store adds `/sm`).
- Both/neither routes configured → hard fail (exit 3).
- Timestamp: `/tr` RFC3161 + `/td SHA256`, URL from
  `COREVIDEO_SIGN_TIMESTAMP_URL` (default `http://timestamp.digicert.com`);
  a timestamp failure fails the sign step — no un-timestamped fallback.
- Post-sign `signtool verify /pa` must pass or the build fails.
- Publisher check reads the MSIX's own AppxManifest (repo
  `Package.appxmanifest` fallback) and hard-fails on mismatch with the cert
  subject (exit 4).
- Exit codes: 2 signtool missing, 3 cert config, 4 publisher mismatch,
  5 sign failed, 6 verify failed, 7 package missing.
- `-DryRun` resolves toolchain + route + publisher check and prints the plan
  without invoking `signtool sign` — that is what
  `scripts/tests/test-sign-native-msix.ps1` asserts against (10 cases,
  including the preserved dev-mode exit-0-with-loud-warning behavior).

### D3 — Installer completeness (first-launch bootstrap, not install-time magic)
MSIX cannot run custom install actions, so the app self-completes on launch —
which also heals broken state on every start:
- **Vcam registration:** move `register-virtualcam.ps1`'s logic in-app (HKCU
  `regsvr32 /s` equivalent via direct registry writes or process spawn), pointed
  at the DLL **in the install dir** (the script only knows `native/build-dev`
  today). Run when vcam is first enabled, or at first launch; re-assert
  idempotently.
- **Recording folder:** already created lazily at runtime — verify on a clean
  profile, no new work expected.
- **Crash dumps:** WER `LocalDumps` needs HKLM/elevation → NOT part of install.
  See S1 for the beta posture (per-user detection of dumps Windows already
  writes; the elevated full-dump setup stays a Diagnostics opt-in button that
  UAC-prompts, reusing `setup-crash-dumps.ps1` with `DumpType=1` minidumps as
  the beta default — full dumps are a dev-rig setting, multi-GB per crash).

### D4 — Update channel
Primary: the `.appinstaller` file with `OnLaunch` update check, hosted at a
stable URL (R2 bucket behind the existing Cloudflare account + a custom domain).
Belt-and-suspenders (and the fallback if D0.2 goes EXE): a startup version check
in the shell against a static `latest.json` at the same host → non-blocking
"update available" bar. No delta/channel logic in beta; one channel.

**Status (2026-07-18): BUILT — hosting decision still the owner's (D0).**
- `scripts/make-appinstaller.mjs` (`npm run make:appinstaller`) generates the
  `.appinstaller` (Identity Name/Publisher parsed from the real
  `Package.appxmanifest`, Version `x.y.z.0`, `<OnLaunch
  HoursBetweenUpdateChecks="0">`) and, with `--latest-json`, the `latest.json`
  feed (`{version, msixUrl, appinstallerUrl, sha256?}`; `--msix-path` adds the
  SHA-256). D5's release pipeline calls it as:
  `node scripts/make-appinstaller.mjs --version <x.y.z> --msix-url <https>
  --appinstaller-url <https> --output <path> [--latest-json <path>]
  [--msix-path <msix>]`. Https URLs and `x.y.z` are validated; failures exit
  non-zero. Vitest-covered (`scripts/make-appinstaller.test.mjs`).
- Shell startup check: `UpdateNotificationService` (WinUI) + `UpdateCheckService`
  / `UpdateFeedParser` / `AppUpdateVersion` / `DismissedUpdateVersionStore`
  (MediaCore, xunit-covered). Reads **`COREVIDEO_UPDATE_FEED_URL`** (the
  `latest.json` URL; **empty/unset = check disabled**, the default until
  hosting exists; https, `file://`, and local paths accepted — the latter two
  for rig tests). Runs once per launch, ~5 s after startup, fully off-thread;
  numeric x.y.z compare (packaged = package identity, unpackaged = assembly
  version); failures are silently logged to `launch.log` (§7: never block or
  nag). A newer version surfaces a static one-shot InfoBar ("Update available —
  vX.Y.Z", "Get update" opens the appinstaller URL in the browser); dismissing
  persists that version in `%LOCALAPPDATA%\CoreVideoPro\update-dismissed.json`
  (standalone flag file, deliberately not ProductionOutputPreferences) and
  suppresses re-showing until a newer version ships.

### D5 — CI release pipeline (tag → release)
**Status: IMPLEMENTED 2026-07-18** (`.github/workflows/release.yml` reworked —
pending #293 (D1 stamp) + #295 (D2 prod sign) merge, the D4 `make-appinstaller`
contract landing, and secrets/vars provisioning: `ZOOM_SDK_URL`, exactly one
signing route, `COREVIDEO_UPDATE_BASE_URL`; the update-host publish step is a
documented fail-soft TODO until the D0/D4 hosting decision). The workflow's
header comment lists every required secret/variable. The old loose-folder CI
job (and its inverted `COREVIDEO_PUBLISH == 'never'` upload gate) is deleted —
nothing consumed its artifact; `npm run pack:native` remains for local use.
`workflow_dispatch` is a build dry-run that emits a loudly-named UNSIGNED
artifact and never creates a release.

Rework `release.yml`: `v*` tag → validate version sync (D1) → windows job:
stage Zoom SDK (per D0.3; if bundling, CI needs the SDK from a **private**
source — private repo release asset or R2, never the public repo) → build
native core (Release, PDBs) → `pack:native:msix` → sign (D2) → create GitHub
Release with `release-notes.mjs` output → upload MSIX + `.appinstaller` +
**symbols bundle** (all PDBs, keyed by version — S1 depends on this) → publish
the `.appinstaller` to the update host. Fix the inverted `COREVIDEO_PUBLISH`
gate. `bump:version` stays manual-before-tag; the pipeline owns everything
after the tag.

## 3. S — Supportability

### S0 — Give telemetry-ingest real storage (prerequisite for S1/S3)
**Status: SHIPPED 2026-07-18** (branch `claude/beta-s0-telemetry-storage`;
contract + deploy steps in `services/telemetry-ingest/README.md`). Crashes →
R2 `REPORTS_BUCKET` at `crashes/<yyyy-mm-dd>/<reportId>.json` (key scheme
already handles S1's `.zip`/`.bin`); every report → KV `REPORTS_KV` index
`report:<reportId>` (timestamp/kind/version/machineClass/size/r2Key; events
≤64KB keep their payload inline in KV — no R2 object per tiny event).
`TELEMETRY_API_KEY` is REQUIRED (unset = loud 500, bad bearer = 401); size
caps 25MB crashes / 64KB events (413); per-IP token bucket 60/min (429,
per-isolate only — limits documented honestly in the worker). API shape
(`/v1/crashes`, `/v1/events`, `{reportId, accepted}`) unchanged; the smoke
script now also posts an event. **Owner action before next deploy:** create
the R2 bucket + KV namespace and paste the namespace id into
`services/telemetry-ingest/wrangler.jsonc` (steps in the service README).

### S1 — Crash pipeline (detect → bundle → offer → upload)
- On launch, scan `%LOCALAPPDATA%\CrashDumps` for new `corevideo-native.exe.*`,
  `CoreVideoPro.WinUI.exe.*`, `corevideo-zoom-engine.exe.*`,
  `corevideo-browser-host.exe.*` dumps since the last-seen watermark (flag file
  in app-data).
- Found → non-blocking InfoBar: "CoreVideo Pro crashed last session — send a
  report?" → assemble zip: dump + `launch.log` + `media-core.log` tail +
  `perf.log` tail + a fresh support bundle JSON → POST `/v1/crashes` (S0) with
  version/machine-class metadata → record reportId for the user to reference.
  Never auto-send; consent per report.
- **Symbols:** D5 uploads per-release PDB bundles; we resolve dumps
  server-side/on the rig with `cdb -y <symbols>;srv*msdl...` (existing recipe).
  The "matching PDB only exists for the CURRENT build" trap from CLAUDE.md is
  exactly what the per-release symbol archive kills.
- Beta default `DumpType=1` (minidump, no elevation needed to *read*; the WER
  key setup itself is the D3 opt-in button). A minidump + our logs resolves the
  0xc000027b and native-crash classes we've actually hit.

### S2 — Support bundle v2 (archive, not just JSON)
Extend `ExportSupportBundleAsync`: write the existing JSON **plus** a zip
containing it + `launch.log` + `media-core.log` (tail ~2MB) + `perf.log` tail +
`vcam-serve.log` if present + recent dump list (names/sizes, not the dumps).
Redaction posture stays builder-side (already tested); add one audit task: grep
the native core's stderr paths for any secret echo, since `media-core.log` is
raw child stderr. In-app: "Export bundle" → open Explorer at the zip + show the
"where to send it" link (B4 feedback channel).

### S3 — Opt-in telemetry events
Settings toggle (default OFF, one line of copy about what's sent). On session
end (and daily heartbeat while running): version, session length, output config
shape (counts/kinds only — no endpoints), crash count since last send, machine
class (GPU tier, CPU cores, RAM band). POST `/v1/events`. This is "is beta
healthy" data, not analytics — keep the payload enumerable in the settings UI.

### S4 — Secrets at rest (cheap, do first)
- `FileZoomTokenStore` already takes encrypt/decrypt delegates: pass DPAPI
  (`ProtectedData`, CurrentUser scope) in `StudioViewModel`'s construction.
  Migration: on load, if plaintext parse succeeds → re-save encrypted.
- Same DPAPI treatment for `StreamRtmpStreamKey` / `StreamSrtPassphrase` inside
  `ProductionOutputPreferencesStore` (prefs version bump + migration).
- Fix the app-return URI discrepancy while in this file set: code default
  `corevideo://oauth/callback` vs manifest/docs `corevideopro://` — verify which
  the broker actually returns to, align constant + `zoomOAuth.json` + manifest,
  and add a test pinning it.

### S5 — Broker + workers ops
The broker code is external (CoreVideo repo) but beta depends on it for sign-in
AND refresh. In this repo: a scheduled uptime check (Worker cron hitting
`/oauth/start` + the three workers' health, alert via email/Discord webhook),
and `smoke:staging-services` wired into CI as a daily scheduled job. Production
domains + deploy hygiene for the broker itself are tracked in beta-plan B2.

## 4. O — Onboarding / first-run

### O1 — Persist what the wizard will set (prerequisite)
Vcam enable/mirror/name move into `ProductionOutputPreferences` (version bump +
migration; they are live-synced but reset every launch today). Without this the
wizard's vcam step is a lie.

### O2 — First-run wizard
First-launch flag in app-data (absence of the flag + absence of prefs = fresh).
A dialog-flow window sequencing EXISTING settings — no new engine surface:
1. Zoom sign-in (`SettingsViewModel.BeginAuthorizationAsync`; skippable)
2. Monitor output device (render-device list; default = OS default)
3. Microphone (explain capture is OFF by default — deliberate posture; offer
   enable + device pick)
4. Cameras: there is NO global camera setting — the step enumerates UVC devices
   and offers "assign to Show Input 1..N" (creates the capture sources the
   Studio already understands). Skippable; zero-camera is valid (Zoom-only show)
5. Recording folder (default `%USERPROFILE%\Videos\CoreVideo Pro`)
6. Virtual camera: enable + name (persisted per O1); note that first enable
   performs the D3 registration
Each step writes through the existing stores immediately — cancel mid-wizard
loses nothing already applied, and the wizard is re-runnable from Settings.

### O3 — Machine-class defaults
Canvas default is already 1080p (verified) — the remaining work is small: gate
the 4K canvas option behind a GPU check (dedicated GPU + VRAM threshold) with a
"your GPU may struggle" warning rather than a hard block, and pick multiview
tile-count defaults by the same probe. One probe service, snapshot value, used
by wizard + settings.

### O4 — Quickstart doc
One page, install → first show, derived from `operator-validation-runbook.md`.
Ships in the repo AND linked from the wizard's last page.

## 5. L — Access control (closed-beta gate)

Use the existing `licensing-api` worker — it is built, KV-backed, and deployed;
wiring beats rewriting:
- **Server:** add `POST /v1/admin/mint` (gated by a separate admin key) so we
  can issue beta keys without Stripe; ensure `LICENSE_API_KEY` is set in prod.
  Entitlement tiers are IGNORED in beta except validity/expiry.
- **Shell:** small `LicenseClient` in MediaCore: activate on first entry
  (`/v1/license/activate` with a machine id), then `/v1/license/verify` at
  startup with a cached last-good verdict and a **7-day offline grace** (beta
  operators run shows in venues with bad network; never block a show on our
  worker's uptime). Invalid/expired → nag banner for 3 launches, then gate.
- **UI:** key entry field in the wizard (O2 step 0) + Settings. No trial, no
  commerce, no watermarking in beta.
- The dormant renderer-side `RemoteLicenseClient`/bridge stays untouched (legacy
  TS path); the native shell talks HTTP directly.

## 6. Sequencing

1. **Now (calendar-bound, no code):** D0.1 signing identity (both routes) +
   D0.3 Zoom SDK question. — the plan's §4.1, unchanged.
2. **First build wave:** S4 (secrets, ~day) → D1 (version stamp) → D2 (prod
   sign mode) → D5 (pipeline) → D4 (appinstaller hosting). Output: a signed,
   updatable artifact from a tag.
3. **Second wave (parallelizable):** S0→S1 (crash pipeline) and S2 (bundle v2)
   | O1→O2→O3 (wizard). D3 (vcam registration in-app) lands with whichever
   wave touches it first.
4. **Third wave:** L (gate) + S3 (telemetry) + S5 (ops cron) as the invite date
   approaches; O4 quickstart last (against the real installer flow).
5. B5 (hardware matrix) and B6 (release gates) run as process throughout —
   the first D5 artifact is what goes on the reference configs.

## 7. Invariants

- **A production pipeline never silently degrades:** signing unavailable =
  build FAILS (kill the exit-0 stub in production mode); unsigned artifacts are
  dev-only and named so.
- **No secrets in plaintext at rest** once S4 lands — new settings carrying
  credentials must use the same DPAPI path; support-bundle redaction tests are
  the template and every new bundle field with secret potential gets one.
- **No elevation in the install path.** Everything install-critical is per-user
  (HKCU vcam, app-data staging); elevation is only ever an opt-in Diagnostics
  action (WER keys).
- **Consent before egress:** crash reports and telemetry are opt-in per-report /
  per-toggle; the payload must be inspectable before send.
- **Never block a show on our backend:** license verify has offline grace; the
  update check is non-blocking; broker outage degrades to cached tokens until
  refresh actually fails.
