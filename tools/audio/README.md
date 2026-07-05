# Audio diagnostic toolkit

Battle-built during the 2026-07-05 click hunt (see `docs/zoom-audio-spec.md` §1 for the
defects these found). Ground-truth instruments: capture what actually plays, scan what
actually flowed — never trust a counter alone, and never declare "clean" from a click
count when the ear says warble (time-warp artifacts have no sample jumps).

| Tool | What it does |
|---|---|
| `loopback-rec.cpp` | WASAPI loopback recorder: captures exactly what a render endpoint plays (what the operator hears) to raw float32. Build: `VsDevCmd -arch=amd64 && cl /EHsc /O2 loopback-rec.cpp ole32.lib`. Run: `loopback-rec <device-substring> <seconds> <out.f32>`. Prints engine discontinuity flags. |
| `click-scan.cjs` | Discontinuity scan for STEREO interleaved f32: click count, positions mod 480/960 (packet/tick fingerprinting), inter-click gaps. `node click-scan.cjs file.f32`. NOTE: false-positives on MONO data — check channel count first (tap files from mono sources are mono). |
| `click-zoom.cjs` | Waveform microscope: prints samples around a position in two files side by side. The artifact's SHAPE names the stage (zeros+attack-ramp = gate; single-sample step = splice; smooth warp = resampler). `node click-zoom.cjs a.f32 b.f32 <frame>`. |
| `timeline-scan.cjs` | Per-10s buckets of RMS + hard/soft(floor) clicks — aligns artifacts to what the operator was doing (the limiter bug fell to this: clicks only in loud buckets). `node timeline-scan.cjs file.f32`. |

Also in the box (in-app, env-gated): set `COREVIDEO_AUDIO_DEBUG_DIR` before launch to get
`tap-in-<source>.f32` (PCM entering the mix), `tap-mon.f32` (MON bus leaving it),
`tap-structure.txt` (per-tick sources/inserts/gains/sends) — splits corruption by stage.
Do NOT leave taps armed for listening sessions (per-tick file I/O on the audio worker is
itself audible — the Heisenberg lesson).

Echo detection: autocorrelation of a loopback capture (see the inline node script in the
2026-07-05 session log / spec O1) — a coefficient ≥0.3 at a stable 20–400ms lag means a
second delayed copy is present (double-routing or endpoint contention).
