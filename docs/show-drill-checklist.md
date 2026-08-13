# Show Drill Checklist — one rehearsal closes the owed rig verifications

_Created 2026-07-19 against main after the #293–#307 merge wave + the two live
fixes (#304 spine/capture, #306 thumbnail storm). Run top to bottom in ONE
session; each line names the PR/gate it closes. The rig build must be merged
main (relaunched 2026-07-19, spine clean, 450/450 native tests)._

## A. Capture lifecycle (#304, #305, #306, G3)

- [ ] Join a real meeting → **Capture** → participant tiles + audio arrive (#304)
- [ ] While captured, play browser/Discord audio for 5+ minutes → **no system
      audio clicking**; corevideo-native CPU well under 1 core (#306)
- [ ] Roster thumbnails update ~2/s (paced by design); program/multiview video
      stays full-rate (#306)
- [ ] **Capture off** → Zoom's recording banner clears within ~2s for
      participants, tiles hold/slate, meeting stays joined; **Capture on** →
      feeds return (#305)
- [ ] Engine off / leave / rejoin mid-show ×3 cycles: no deadlock, no orphan
      state, UI never freezes (leave no longer blocks the UI thread) (#302, G3)
- [ ] App exit while in a meeting → clean shutdown, no hang (#302)

## B. Audio (#309 pending merge, G3 console legs)

- [ ] Console walkthrough: fader/pan/mute/solo live, gate-threshold drag audible
      while someone talks, meters + LUFS moving (owed since C7)
- [ ] Mastering enabled on master: target holds, program L/R inherit
- [ ] After #309 merges: listening pass — single-band vs multiband A/B on real
      program; no zipper when the true-peak ceiling engages; a recording scans
      ≤ −1.3 dBTP
- [ ] Vcam consumed in Zoom + browser audio → still clean (G0 re-confirm on the
      new build)

## C. A/V sync (G2 — north star)

- [ ] Clap test on a real recording: head AND tail offset < 50ms
- [ ] 5-minute recording: audio track present (aac 48k stereo), duration delta
      < 200ms (the owed #163/G2 real-meeting leg)

## D. The triple output (G3 core)

- [ ] Record 1080p MP4 + RTMP push + virtual camera in Zoom **simultaneously**,
      30+ min: no artifacts, working sets flat, drops 0, RTMP sustained (#288)
- [ ] Vcam prefs: enable + name + mirror → restart app → camera returns with the
      same state (#307)

## E. Media & graphics (owner list 2026-07-19)

- [ ] Still logo/bug composites real pixels on preview AND program (#285/#283)
- [ ] Lower thirds + captions + clock render at 60fps over program
- [ ] Browser source: add a URL → real pixels composite; reload works; kill the
      host in Task Manager → held frame → slate → respawn (#287)
- [ ] DSK workflow: key bus on/off during program (#289 — never exercised by
      anyone but us)

## F. Supportability (new this wave)

- [ ] Export support bundle → zip appears beside JSON with log tails +
      manifest; Explorer opens at it; spot-check no secrets in tails (#296)
- [ ] Sign in → restart → still signed in; `zoom-oauth.json` shows `dpapi:`
      fields; stream key in prefs encrypted (#297)
- [ ] Drop a fake `corevideo-native.exe.12345.dmp` in `%LOCALAPPDATA%\CrashDumps`
      → next launch shows the crash-report InfoBar; **Send** against a local
      `wrangler dev` telemetry worker → 202 + reportId (#301/#294)
- [ ] Point `COREVIDEO_UPDATE_FEED_URL` at a local `latest.json` with 9.9.9 →
      update banner appears; unreachable feed → silent (#299)

## G. Packaged run (G5 — verify, don't assume)

- [ ] `npm run pack:native:msix` → install on a clean profile → app launches,
      Zoom runtime discovered, core starts, recording folder created
- [ ] Packaged sign-in completes (the `corevideo://` manifest fix, #297 — this
      was impossible before)
- [ ] Packaged recording has an audio track (the 07-02 evidence gap, G2)

## Out of band (not rig work)

- Start the signing identity validation (Azure Trusted Signing + OV cert) and
  the Zoom SDK redistribution question — calendar-bound, gate the first
  external install (beta D0)
- Provision CI secrets per #300's table; create the R2 bucket + KV namespace
  and deploy telemetry-ingest per #294's README
- One elevated `scripts/setup-crash-dumps.ps1` run on this rig (G4, still owed)
- Resize soak under load (G4 — the 0xc000027b mitigation is by-design but not
  soak-verified)
