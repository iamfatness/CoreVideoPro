# Live session findings — 2026-09-13

Metrics captured from a real ~5-hour test-meeting session (grew 10 → 16 participants,
8-member Tiles wall at 1080p, streaming to YouTube), read live from `/snapshot`,
`media-core.log`, and `launch.log`. Build under test: `821f950e` (main + the tiles-1080p
resolution change + the #513 crash-doc), running as the cap-12 test build.

## Session metrics

| Area | Reading | Verdict |
|---|---|---|
| Core render / video-out / audio worker | 60.1 / 60.1 / 50.1 per s | healthy at rate |
| Program buffer | 13 underruns, 6 overflows total; 0 in the sampled windows | hitches during heavy windows only |
| Render deadline misses | 1339 total (~8/min) | low-grade; the 3-frame buffer absorbs |
| Monitor-shed | entered **once** (09:47–09:52), reached divisor 3, **programMs peaked 16.85 ms** vs 16.67 budget | at the load ceiling at 16 participants |
| coreMutex holds | one **143 ms** `cmd.handle` (11:38); render.display-tick otherwise <9 ms (2 over 1 frame, 1 = the 44.8 ms take) | one real ~130 ms freeze |
| Zoom subscription churn | 46 total, **0/min at sampling** | tiles-1080p fix holding — no preview churn |
| Camera-on participants | **9** | exceeds the shipped 1080p cap of 8 |
| Master audio | −16.9 LUFS integrated, −1.8 dBTP | healthy loudness |
| Shell structural rebuild | **17–21 ms**, 5× this session, on roster changes | over frame budget (#509) |
| Shell exceptions / crashes | 0 | no fail-fast this session (#513 did not recur) |
| Streaming to YouTube | 47/60 fps delivered; FFmpeg lag 0.7 s → 124 s | falling behind live (the reported incident) |

## Things to address

### P0 — show-affecting, in flight
- **Streaming falls behind live** ("poor / not enough data"): FFmpeg `-re` on the live
  video pipe turned RTMP backpressure into unbounded lag (0.7 s → 124 s; 47/60 fps).
  Fix prepped: **PR #515** (drop `-re` on video → drop-to-live). Needs a live soak.
- **Tiles dropping when previewing**: fixed and owner-confirmed — **PR #514** (wall at
  1080p, no resolution flip). Cap caveat below.

### P1 — surfaced by this session's load
- **#516** — a command held `coreMutex` for **143 ms** (budget 50 ms) at 11:38, a
  ~130 ms on-air freeze. One-off; needs the command named and its heavy work moved off
  the render lock.
- **#517** — **Program render hits the one-frame budget at 16 participants** (programMs
  16.85 ms, tripped the monitor-shed to divisor 3). The wall at 1080p × a full room is at
  the ceiling; needs render headroom before rooms scale further.
- **#509** — **shell structural rebuild is 17–21 ms** on every roster change, dominated by
  `productionReadouts 6.8 + showInputEditors 4.4 + gallery 4.3 + audioRows 4.0` ms.
  Now measured on the owner machine (comment added): it is the sum of four moderate
  projections, not one 197 ms step; trigger confirmed as participants leaving. Make them
  diff-based.

### P2 — verify / cleanup
- **#518** — **verify Zoom audio on air**: FADER LAW dropped `zoom-mix` as unrouted.
  Master shows −16.9 LUFS so audio is present, but confirm the intended Zoom path reaches
  the stream in the current audio mode; downgrade the warning in perGuestIso if expected.
- **#519** — **fake sender counters**: `bytesSent` is estimated (assumes 30 fps) and
  `latencyMs` is hard-coded 2100. Both read as measurements in `/snapshot` and misled the
  streaming diagnosis. Make real or remove.

### Cap decision (relates to PR #514)
The soak proved **8** concurrent 1080p on the GPU pipeline, and the shipped cap is 8. This
session reached **9 camera-on**, so the shipped cap would demote one participant to 720p.
Either re-soak to raise the cap to 9–10, or accept a graceful single-tile demotion above 8.

### Already tracked, still open
- **#508** — multiview bottom-row click/label misalignment (root cause: half-tile centering
  of a partial last row; analysis on the issue).
- **#513** — idle GC-finalizer XAML-release crash (0xc000027b); did not recur this session.

## Counters that lie (do not trust in future diagnosis)
- `outputSenderSession.senders[].bytesSent` — estimated, not measured (#519).
- `outputSenderSession.senders[].latencyMs` — hard-coded 2100 (#519).
The trustworthy streaming signals are `framesSent` rate and FFmpeg's own stderr.
