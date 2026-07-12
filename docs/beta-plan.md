# CoreVideo Pro - Beta Plan

_Created 2026-07-12. Assumes the alpha gates in `docs/alpha-plan.md` have passed:
the owner runs real shows on this rig reliably. Beta changes the question from
"does the product work?" to "can someone who is not us install it, get to a show,
and be supported when something breaks?"_

## 1. What beta means

**Beta = a small set of external operators (5-20) run real shows on their own
machines, with us able to diagnose failures we did not witness.** Every beta
workstream below serves one of those three clauses: install (B1), get to a show
(B2/B3), be supported (B4/B5/B6).

## 2. Workstreams

### B1 - Distribution
The single largest absent-entirely gap: today there is no signed artifact, no real
installer, and no update mechanism.
- [ ] Production code signing: Authenticode cert (org validation takes weeks - START
      FIRST); decide MSIX-signed vs signed installer EXE
- [ ] Real installer: stages app + native core + FFmpeg runtime, registers the vcam
      DLL (HKCU, no elevation), sets up crash dumps, creates recording folder
- [ ] Auto-update channel (even minimal: startup version check + download prompt);
      beta iterates fast, manual re-install kills participation
- [ ] Zoom SDK redistribution: confirm license terms permit shipping SDK binaries in
      the installer (per `docs/zoom-windows-sdk-packaging.md` this is unresolved)
- [ ] CI release pipeline: tag -> build -> sign -> package -> release notes
      (`bump:version` / `release:notes` exist; wire them together)

### B2 - Onboarding / first-run (absent entirely today)
- [ ] First-run wizard: Zoom sign-in -> audio devices (monitor output, mic) -> cameras
      -> recording folder -> vcam enable. Every step already exists as a setting;
      the wizard is sequencing + defaults
- [ ] Productionize the OAuth broker (currently dev-hosted at
      `corevideo.iamfatness.us`): stable domain, monitoring, and the token-refresh
      fallback from alpha G4
- [ ] Sane first-launch defaults per machine class (canvas/render resolution by GPU -
      4K canvas is an RTX-4090 assumption; pick 1080p defaults on lesser GPUs)
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
      output configuration - enough to know if beta is healthy without a support ticket
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
      fallback-to-bridge path

### B6 - Release gates (per beta build)
Turn the existing tooling into a repeatable pre-release checklist:
- [ ] Audio soak (fake engine, clicks:0) - exists, keep green
- [ ] A/V load soak: record + RTMP + vcam simultaneously, 60+ min, flat memory,
      0 drops (the alpha G3 drill, automated where possible)
- [ ] Churn soak: join/leave/rejoin + window resize + device unplug under load
      (the 0xc000027b class)
- [ ] Clean-box install -> first-run wizard -> 5-minute show on a reference config

### B7 - Feature cut line (owner decision)
Recommended IN for beta (differentiation + already close):
- **VST P2c** - real third-party plugin processing (host transport + probe are
  shipped; P2c needs a probe-passing plugin installed to verify, e.g. TDR Nova).
  "Runs your plugins" is a headline capability vs Ecamm/mimoLive
- **Vcam true 60fps** (if not done in alpha stretch)
- **Mastering reference presets** (analysis shipped in #246; needs file-decode wiring
  + listening pass) - "sound like X" is a demo-able wow feature

Recommended OUT for beta (post-beta / 1.0):
- Browser sources (WebView2 host - big security/stability surface)
- NDI hardening, SRT, DeckLink/AJA (pro-IO users can wait for 1.0; vcam covers the
  "get program into another app" need)
- V5 virtual microphone
- Per-source sync offsets (needs the delay-line work + owner ears)
- ASIO capture

## 3. Sequencing

1. **Start immediately regardless of alpha state:** the Authenticode cert purchase
   (calendar time) and the Zoom SDK redistribution question (legal/licensing, may
   reshape the installer).
2. **B1 + B4 first** - they de-risk everything else: a signed, updatable, crash-
   reporting build is the platform the rest of beta iterates on.
3. **B2 + B5 together** - the first-run wizard is designed against the reference
   configs, not the dev rig.
4. **B3 + B6** as the beta-invite date approaches.
5. **B7 features** ride whichever release train they are ready for; none block the
   first external install.

## 4. Beta exit (what 1.0 asks)

Not specced here; roughly: open distribution, commerce/licensing for real, the
post-beta feature list above, and a support load we can carry. Write it when beta
teaches us what actually breaks.
