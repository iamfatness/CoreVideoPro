# CoreVideo Pro - Beta Plan

_Created 2026-07-12; rewritten 2026-07-18 after the alpha gates were re-scored (PRs
#280, #286-#291, and the owner's 30-minute large soak completed 2026-07-18). Beta
changes the question from "does the product work?" to "can someone who is not us
install it, get to a show, and be supported when something breaks?"_

_The engineering design for the build-workstreams (B1 distribution, B2 onboarding,
B3 access control, B4 supportability) lives in `docs/beta-engineering-spec.md`
(written 2026-07-18 against a same-day audit of the packaging scripts, CI, the
Cloudflare workers, and the shell's settings/support surface)._

## 1. What beta means

**Beta = a small set of external operators (5-20) run real shows on their own
machines, with us able to diagnose failures we did not witness.** Every beta
workstream below serves one of those three clauses: install (B1), get to a show
(B2/B3), be supported (B4/B5/B6).

## 2. Where we stand: alpha gate scoreboard (2026-07-18)

The alpha plan (`docs/alpha-plan.md`) is verification-and-stability work; here is
what is actually left of it. Beta prep starts NOW in parallel — nothing below in
§3 waits on these, but the beta invite date does.

| Gate | Status | Remaining |
|---|---|---|
| G0 system-audio citizenship | Fixes shipped 07-12 | Owner verdict: CVP vcam in Zoom + browser audio clean (if the 07-18 soak ran this combo, record the verdict and close) |
| G1 native UVC default-ON | **SHIPPED** #280 (default-ON, `COREVIDEO_NATIVE_UVC=0` opts out) | Owner visual confirm on all real cameras; PresentMon re-run under show load (closes the P4 question) |
| G2 A/V sync | Zero-audio bug fixed #286; headless 60s proof green (1.8ms/123ms) | Rig clap test head+tail; **packaged-run** recording has audio (verify, don't assume) |
| G3 show drill | **30-min soak DONE 2026-07-18**; sustained RTMP fixed #288 | Confirm the soak ran record+RTMP+vcam simultaneously; engine-off/leave/rejoin mid-show; support bundle export after the run |
| G4 stability debt | OAuth refresh **SHIPPED** #290 (broker `/oauth/refresh`) | Engine-off teardown audit vs the five ZoomISO deadlock rules; resize soak under load; expired-token rig check; one elevated `setup-crash-dumps.ps1` run |
| G5 packaging-lite | Scripts exist (`package-native-msix.ps1`, `sign-native-msix.ps1`) | Clean-profile packaged launch: Zoom runtime discovery, core launch, recording folder, vcam registration; alpha release note |

Feature deltas since the 07-12 plan that reshape beta scope (see B7):
**VST3 is real** — live isolated plugin processing + plugin editors shipped (#291);
**mastering A-B + presets shipped** (#290/#291); **browser sources BR-1 shipped**
(#287, was "recommended OUT"); **DSK workflow + independent key buses shipped**
(#289) — none of these were in the 07-12 cut-line math.

## 3. Workstreams

### B1 - Distribution (THE critical path)
The single largest absent-entirely gap: no signed artifact, no real installer, no
update mechanism. Two items here are calendar-bound and start immediately.
- [ ] **START NOW:** Production code signing: Authenticode cert (org validation
      takes weeks); decide MSIX-signed vs signed installer EXE
- [ ] **START NOW:** Zoom SDK redistribution: confirm license terms permit shipping
      SDK binaries in the installer (`docs/zoom-windows-sdk-packaging.md` marks this
      unresolved; the answer may reshape the installer — e.g. a first-run SDK
      download step instead of bundling)
- [ ] Real installer: stages app + native core + FFmpeg runtime, registers the vcam
      DLL (HKCU, no elevation), sets up crash dumps, creates recording folder
- [ ] Auto-update channel (even minimal: startup version check + download prompt);
      beta iterates fast, manual re-install kills participation
- [ ] CI release pipeline: tag -> build -> sign -> package -> release notes
      (`bump:version` / `release:notes` exist; wire them together)

### B2 - Onboarding / first-run (absent entirely today)
- [ ] First-run wizard: Zoom sign-in -> audio devices (monitor output, mic) -> cameras
      -> recording folder -> vcam enable. Every step already exists as a setting;
      the wizard is sequencing + defaults
- [ ] Productionize the OAuth broker (currently dev-hosted at
      `corevideo.iamfatness.us`): stable domain, monitoring, uptime alerting. The
      token-refresh fallback itself is DONE (#290) — the broker is now a hard
      runtime dependency for every beta user's sign-in AND refresh, which raises
      the availability bar
- [ ] Sane first-launch defaults per machine class (audited 2026-07-18: the canvas
      default is ALREADY 1080p — `MediaCoreProductionSyncContext` — so the remaining
      work is gating the 4K option + multiview tile counts behind a GPU probe, not
      changing defaults; see spec §O3)
- [ ] Quickstart doc: one page from install to first show (derive from
      `docs/operator-validation-runbook.md`)

### B3 - Access control
Keep it minimal for beta; this is a gate, not a store.
- [ ] Closed-beta gate: license key or email allowlist checked at startup.
      `services/licensing-api` (Cloudflare Worker) exists but is only wired to the
      legacy TS engine - either wire the native shell to it or ship a simpler
      signed-key check and defer the service
- [ ] No trial/commerce mechanics in beta

### B4 - Supportability
The diagnosability investment (PDBs, WER dumps, support bundle) exists on the dev
rig; beta needs it to work on machines we cannot touch.
- [ ] Crash pipeline: installer enables WER LocalDumps; app detects a new dump on
      next launch and offers "send report" (bundle dump + logs); symbol store per
      release so dumps resolve
- [ ] Support bundle: verify redaction (stream keys, tokens), size sanity, and an
      in-app "export + where to send it" flow
- [ ] Basic anonymous telemetry (opt-in): version, session length, crash count,
      output configuration (`services/telemetry-ingest` exists — wire the shell to
      it) - enough to know if beta is healthy without a support ticket
- [ ] Feedback channel: in-app link (Discord/email/form - owner picks)

### B5 - Hardware compatibility matrix
Everything so far is proven on ONE machine (RTX 4090, GoXLR, Elgato). Beta's main
technical risk is machine variance, especially GPU tier and audio-device weirdness.
- [ ] Acquire/borrow 2-3 reference configs: mid-tier desktop GPU, a laptop with
      hybrid graphics, one AMD GPU
- [ ] Define minimum spec + auto-scale (render resolution, multiview tile count,
      canvas default) instead of hard failure
- [ ] Audio device sweep: default endpoints, USB interfaces, Bluetooth (likely
      "unsupported for monitoring" - say so explicitly), device unplug/replug
      mid-show
- [ ] Camera sweep beyond Elgato: cheap UVC webcams exercise the native-UVC
      fallback-to-bridge path (now the DEFAULT path for every beta user — the
      per-device bridge fallback must be bulletproof, not just present)
- [ ] WebView2 Runtime presence check on clean machines (browser-host exits 3 when
      missing — surface that as a friendly message, not a dead source)

### B6 - Release gates (per beta build)
Turn the existing tooling into a repeatable pre-release checklist. The 07-18 soak
proves the bar is reachable; the work is making it repeatable per build.
- [ ] Audio soak (fake engine, clicks:0) - exists, keep green
- [ ] Recording-audio proof (`node scripts/validate-record-audio.mjs`) - exists,
      keep green; it guards the exact 0xC00D36B3 regression class
- [ ] A/V load soak: record + RTMP + vcam simultaneously, 60+ min, flat memory,
      0 drops (the alpha G3 drill — done once manually 2026-07-18 at 30 min;
      script the setup via the control API so re-running it is one command, and
      stretch to 60 min for release gating)
- [ ] Churn soak: join/leave/rejoin + window resize + device unplug under load
      (the 0xc000027b class — resize mitigation is by-design but still not
      soak-verified; this is the alpha G4 carryover)
- [ ] Clean-box install -> first-run wizard -> 5-minute show on a reference config

### B7 - Feature cut line (owner decision — re-scored 2026-07-18)
Resolved since 07-12 (shipped, so no longer decisions — now beta test surface):
- **VST3 processing** — SHIPPED (#291: live isolated VST3 processing + plugin
  editors; P2c core landed #284). "Runs your plugins" is a headline capability vs
  Ecamm/mimoLive. Beta needs: a plugin compatibility shortlist (verify 3-5 popular
  free/paid VST3s, e.g. TDR Nova), and the crash posture re-proven per plugin
  (bypass-on-host-death is the design; beta users WILL load cursed plugins)
- **Mastering A-B + presets** — SHIPPED (#290/#291). Remaining: owner listening
  pass on the presets before they're demo'd as a wow feature
- **DSK workflow + key buses** — SHIPPED (#289, unplanned). Include in the beta
  test script and quickstart; it's operator-facing surface that has never been
  exercised by anyone but us

Still-open decisions:
- **Browser sources (BR-1)** — SHIPPED #287, but the 07-12 plan recommended OUT for
  beta on security/stability grounds and those grounds haven't changed (a WebView2
  process per source, unrestricted navigation). Recommendation: ship it **enabled
  but labeled experimental** in beta — supervision (backoff, give-up-after-5,
  loud health) already exists, removal now costs more than containment, and real
  overlays (tickers, score bugs) are exactly what beta operators will try. Fallback
  position if it misbehaves on the matrix: settings toggle default-off
- **Vcam true 60fps** — ~50fps today is competitive for a webcam consumer; the
  remaining work is GPU NV12 in `exportVcamSharedTexture`. IN if cheap, not gating
- **Mastering reference presets from file analysis** (#246 analysis shipped; needs
  file-decode wiring + listening pass) — nice demo, not gating

Recommended OUT for beta (post-beta / 1.0) — unchanged:
- NDI hardening, SRT, DeckLink/AJA (vcam covers "get program into another app")
- V5 virtual microphone
- Per-source sync offsets (needs the delay-line work + owner ears)
- ASIO capture
- Browser sources BR-2/BR-3 (interactivity, page audio) — BR-1 render-only is the
  beta surface

## 4. Sequencing

1. **This week (parallel with alpha close-out):** buy the Authenticode cert and
   open the Zoom SDK redistribution question — both are calendar-bound and gate
   the first external install. Neither needs any code.
2. **Alpha close-out (short list, §2):** the owed rig verifications — G0 verdict,
   G2 clap + packaged-run check, G3 rejoin/support-bundle legs, G4 teardown audit
   + resize soak, G5 clean-profile packaged launch. Batch them into ONE more
   structured rehearsal + one packaged-build session rather than piecemeal.
3. **B1 + B4 first** - they de-risk everything else: a signed, updatable, crash-
   reporting build is the platform the rest of beta iterates on.
4. **B2 + B5 together** - the first-run wizard is designed against the reference
   configs, not the dev rig. Start acquiring the reference hardware now; it has
   lead time too.
5. **B3 + B6** as the beta-invite date approaches. B6's soaks should be scripted
   versions of what the 07-18 run did manually.
6. **B7 features** ride whichever release train they are ready for; none block the
   first external install.

## 5. Beta exit (what 1.0 asks)

Not specced here; roughly: open distribution, commerce/licensing for real, the
post-beta feature list above, and a support load we can carry. Write it when beta
teaches us what actually breaks.
