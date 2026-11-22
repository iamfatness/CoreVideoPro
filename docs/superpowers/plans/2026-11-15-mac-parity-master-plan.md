# macOS feature parity with CoreVideo Pro 1.0: master plan (2026-11-15)

Status: DRAFT for owner review. Supersedes the milestone list in
`2026-11-14-mac-parity-1.0-roadmap.md` (its owner decisions in §5 and rules in §6 still stand).

## 1. What this is

Ten design specs, written 2026-11-15 from the code on branch `mac-tier0b`, cover every open gap
between the Mac shell and Windows 1.0: 106 slices in all. This document sequences them.
Each spec carries the Windows behaviour to match, the Mac state today, the design, slices,
tests, risks and owner decisions, with `file:line` evidence.

**Evidence level.** The specs come from reading code. Nothing in them was built or run, and
each marks what it could not verify. Sizes are the spec authors' estimates for one engineer
lane (S up to 1 day, M 2-4 days, L 1-2 weeks, XL 2-4 weeks).

| # | Workstream | Priority | Spec | Slices | Effort |
|---|---|---|---|---|---|
| 1 | Media clip playback | Owner priority 1 | `docs/superpowers/specs/2026-11-15-mac-media-clip-playback-design.md` | 8 | Native 4-5 weeks, shell 1.5-2 weeks, in parallel |
| 2 | Sources screen rebuild | Owner priority 2 | `docs/superpowers/specs/2026-11-15-mac-sources-rebuild-design.md` | 6 | Shell 4-5 weeks, native about 1 week |
| 3 | Per-source colour grading and scopes | Owner priority 2 | `docs/superpowers/specs/2026-11-15-mac-source-grading-scopes-design.md` | 8 | Shell 2.5-3 weeks, native 3-4 weeks, in parallel after G1 |
| 4 | Remote control parity | Owner priority 3 | `docs/superpowers/specs/2026-11-15-mac-remote-control-parity-design.md` | 7 | 2.5-3 weeks for RC1-RC5, then 2-3 weeks spread across other workstreams |
| 5 | Metal graphics backend | Owner priority 4 | `docs/superpowers/specs/2026-11-15-mac-metal-graphics-backend-design.md` | 6 | Native about 5 weeks (G1-G5) |
| 6 | Graphics in the shell | Owner priority 4 | `docs/superpowers/specs/2026-11-15-mac-graphics-shell-design.md` | 15 | Shell 13-17 weeks (Operate 5-6, builder and feeds 3-4, Designer 5-7) |
| 7 | CoreVideo Tiles | Owner priority 4 | `docs/superpowers/specs/2026-11-15-mac-tiles-design.md` | 10 | Shell 5-6 weeks, native 1-1.5 weeks |
| 8 | Audio parity | After the big four | `docs/superpowers/specs/2026-11-15-mac-audio-parity-design.md` | 13 | About 5-6 weeks for A1-A9; VST3 adds 5-6 weeks native |
| 9 | Outputs, network ingest and browser sources | After the big four | `docs/superpowers/specs/2026-11-15-mac-outputs-ingest-browser-design.md` | 16 | 9-11 weeks in one lane, about 6 with native and shell in parallel |
| 10 | Comms, social review and remaining items | After the big four | `docs/superpowers/specs/2026-11-15-mac-comms-social-remaining-design.md` | 17 | Native about 3 weeks, shell 10-11 weeks |

**Total:** about 30 native lane-weeks and 54 shell and control lane-weeks.
With one native lane and two shell lanes that is roughly 27-30 weeks of calendar time to full
parity, or about 22-24 weeks to the end of Phase 4 if the Designer and VST3 follow a release.
The 11-14 roadmap's estimate of 5-7 weeks per lane was too low: it predated the
control-by-control UI audits and had no audio milestone.

## 2. Phases

Lanes run in parallel inside a phase. A phase ends with a signed, notarized .pkg and an owner check.

### Phase 0: Now: land tonight's work

Installer 5 (virtual camera fix) installed and proven live; Tier 0 show-breakers and the virtual camera merged from branch mac-tier0b.

**Outcome:** Owner install + Photo Booth and Zoom check.

### Phase 1: Media and Sources

Clips play on air with audio. The Sources screen obeys the slot law and shows real signal facts. Basic per-source grading. Remote control grows from 13 to 37 actions plus OSC. Quick wins in audio and output settings.

**Outcome:** First build an operator can run a clip-and-guests show on.

| Workstream | Slice | What lands | Lane | Size | Depends on |
|---|---|---|---|---|---|
| Media clip playback | N1 | AVFoundation video decoder (BGRA) + video scene backgrounds | native | L | none |
| Media clip playback | S1 | mediaSources decode, Cue in Preview, status, wire cleanup | shell | M | N1 for clips |
| Media clip playback | S3 | Bin parity: filter, grouping, duration, Select, Remove, local preview | shell | M | none |
| Media clip playback | N2 | Clip audio into the Media strip | native | M | N1 |
| Media clip playback | S2 | Play/Pause gesture, tap rules, Take promotion and rollback | shell | M | N1, S1 |
| Sources screen rebuild | S1 | Slot law: newcomer-only auto-assign, reserved slots, toggle, immediate save | shell | M | none |
| Sources screen rebuild | S2 | Observed row facts + chips on existing rows | shell | M | none |
| Sources screen rebuild | S3 | Full-width table, filters, capture section, labelled types | shell | L | S2 |
| Sources screen rebuild | S5 | Inspector: names, dropout, role, lower-third text, Key, Grade slot | shell | L | S3 |
| Per-source colour grading and scopes | G0 | Grade identity on the wire (query-source-grade + sourceGrades node) | native (shared) | S | none |
| Per-source colour grading and scopes | G1 | Per-source basic grade + looks, store, persistence, re-assert | shell | M | G0 |
| Per-source colour grading and scopes | G2 | Advanced stack, undo, copy/paste, presets | shell | M | G1 |
| Remote control parity | RC1 | Shared manifest, generated Swift catalog, closure test, fixtures on both hosts | control | M | none |
| Remote control parity | RC2 | Session and inputs (12 actions, state fields) | control | M | RC1 |
| Remote control parity | RC3 | Audio, multiview, legacy overlays (12 actions) | control | M | RC1 |
| Remote control parity | RC4 | OSC codec, router, UDP server, feedback, Settings status row | control | M | RC1 |
| Audio parity | A1 | Zoom audio mode toggle + persistence | shell | M | none |
| Audio parity | A2 | Mastering: 21 fields, persisted, limiter toggle | shell | M | none |
| Audio parity | A5 | Local audio source (mic or interface) | shell | S | none |
| Outputs, network ingest and browser sources | Z1 | Zoom quality pickers | shell | S | none |
| Outputs, network ingest and browser sources | Z2 | Capture audio-sync nudge | shell | S | none |
| Outputs, network ingest and browser sources | O1 | Output profiles + advanced stream settings | shell | M | none |
| Outputs, network ingest and browser sources | O2 | AVFoundation scale-not-crop, HEVC recording | native | M | none |

### Phase 2: Control and grading depth

Companion verified on Mac. Thumbnails and tally on Sources. Curves, LUTs, private grade preview and scopes. Streaming without Homebrew, with truthful destination status. Mastering presets, loudness meters and all buses.

**Outcome:** Stream Deck operation; colourist tools; dependable streaming.

| Workstream | Slice | What lands | Lane | Size | Depends on |
|---|---|---|---|---|---|
| Media clip playback | N3 | Audio-only assets, alpha, rotation | native | S-M | N2 |
| Media clip playback | N4 | Zero-copy IOSurface frames into Metal | native | L | N1 |
| Sources screen rebuild | S4 | Stills as slot sources (clips once media lands) | shell | S | S3, Media |
| Sources screen rebuild | S6 | Source sheet thumbnails + tally | native+shell | L | S3 |
| Per-source colour grading and scopes | G3 | Curves editor + .cube import | shell | M | G2 |
| Per-source colour grading and scopes | G4 | Hook into the Sources inspector and toolbar | shell | S | G1, Sources S5 |
| Per-source colour grading and scopes | G5 | Metal preview worker + compare | native+shell | L | G1 |
| Per-source colour grading and scopes | G6 | Metal scopes + scope panel | native+shell | L | G5 |
| Per-source colour grading and scopes | G7 | Region of interest | shell | M | G6 |
| Remote control parity | RC5 | Companion module verified on Mac | control | S | RC2, RC3 |
| Remote control parity | RC6 | Volt rundown (3 actions) | control | S | Media |
| Audio parity | A3 | Mastering presets, A/B compare | shell | M | A2 |
| Audio parity | A4 | Meters: integrated LUFS, true peak, stereo rail, gain reduction | shell | M | A2 |
| Audio parity | A6 | Buses: ISO 2-8, aux, custom, bus sends, LISTEN | shell | M | none |
| Outputs, network ingest and browser sources | F1 | Bundled LGPL ffmpeg | native+release | M | ffmpeg decision |
| Outputs, network ingest and browser sources | P1 | POSIX child lifetime (no orphaned helpers) | native | M | none |
| Outputs, network ingest and browser sources | D1 | Destinations list (RTMP/SRT/HLS), truthful status, Retry | shell | M | F1, P1 |
| Comms, social review and remaining items | R1 | Input rows projection + format chips | shell | M | none |
| Comms, social review and remaining items | R2 | REC chip, cap note, row preview | shell | M | R1 |
| Comms, social review and remaining items | R3 | Subscription evidence test + Health row | native+shell | S | none |

### Phase 3: Graphics and Tiles

Metal graphics backend. Graphics Operate: playlist, On Air strip, F-keys, datasets, crawls, Shot Layouts. Tiles wall with inspector.

**Outcome:** Graphics and Tiles parity for operating a show (looks authored on Windows).

| Workstream | Slice | What lands | Lane | Size | Depends on |
|---|---|---|---|---|---|
| Metal graphics backend | G1 | Async Core Text raster; legacy overlays converge; slate name label | native | M | none |
| Metal graphics backend | G2 | Template raster, text measurement, font resolver, facts and warnings, capability on | native | L | G1, font decision |
| Metal graphics backend | G3 | Bounds and failure behaviour (capacity, font repair, cache pressure) | native | M | G2 |
| Metal graphics backend | G4 | Parity reference, Mac goldens, drill with a graphics cue | native | M | G2, one Windows session |
| Metal graphics backend | G5 | Library thumbnails on Metal | native | S | G2 |
| Graphics in the shell | G1 | Gateway, snapshot decode, On Air strip, legacy drawer, Keychain helper | shell | M | Metal (live check only) |
| Graphics in the shell | G2 | Document store, playlist Cue/Show/Hide, inspector, Guest rows | shell | L | G1, Metal |
| Graphics in the shell | G3 | F-keys, cue rides Take, lower-third authority | shell | M | G2 |
| Graphics in the shell | G4 | Names list, clocks | shell | S | G2 |
| Graphics in the shell | G5 | Local datasets | shell | M | G2, Metal tables |
| Graphics in the shell | G6 | Crawls | shell | M | G5, Metal crawl |
| Graphics in the shell | G7 | Shot Layouts, guarded Take | shell | L | G3, G5 |
| CoreVideo Tiles | N1 | Metal Tiles pixel proofs + fixes | native | S-M | none |
| CoreVideo Tiles | S1 | Tiles scene end to end (model, payload, starter, empty routes) | shell | M | N1 |
| CoreVideo Tiles | S2 | Subscriptions: Tiles tiers, sticky audio, source ids, shortfall | shell | M | S1 |
| CoreVideo Tiles | S3 | Identity + membership modes, manual slots, never-show | shell | M | S1 |
| CoreVideo Tiles | S4 | Inspector: Edit Tiles, Layout + Style, presets, colour pickers | shell | L | S1 |
| CoreVideo Tiles | S5 | Members tab: live badges, pin/crop/order, live background | shell | M | S3, S4 |
| CoreVideo Tiles | A1 | Drill + live acceptance | native+owner | S | S2, S4 |
| Comms, social review and remaining items | B1 | Shared Keychain helper | shell | S | none |

### Phase 4: Breadth

Comms talkback. Social review with YouTube and Twitch. SRT/RTMP ingest, NDI output, browser overlays, GPU-direct encode. Design Builder and feeds. Remaining control actions. Telemetry and crash upload. Leftover audit rows.

**Outcome:** Everything an operator can do on Windows except the items in the last phase.

| Workstream | Slice | What lands | Lane | Size | Depends on |
|---|---|---|---|---|---|
| Remote control parity | RC7 | Follow-on batches per unblocked workstream (71 actions) | control | S-M each | named specs |
| Graphics in the shell | G8 | Design Builder | shell | L | G2, Metal thumbnails |
| Graphics in the shell | G9 | HTTPS feeds, custom API, credentials | shell | M | G5, G1 |
| Graphics in the shell | G10 | WebSocket push feeds | shell | M | G9 |
| Graphics in the shell | G11 | graphics.* control actions | control | M | G3, G7, Control |
| CoreVideo Tiles | N2 | Metal overlay raster cache; enable 'Name each tile' | native+shell | M | Metal G1, S4 |
| CoreVideo Tiles | C1 | Eight Tiles control actions | control | S | Control, S3, S5 |
| CoreVideo Tiles | S6 | Wall as a layer; authored layers over and under the wall | shell | M | Sources, S4 |
| Audio parity | A7 | Processing workspace: bus and master chains, insert list, curves | shell | L | A6 |
| Audio parity | A8 | System audio via a Core Audio process tap | native+shell | L | A5, spike |
| Audio parity | A9 | Sync trims: guest lip sync, capture nudge | shell | M | A1 |
| Outputs, network ingest and browser sources | O3 | HEVC VideoToolbox policy + real probe | native | S | F1 |
| Outputs, network ingest and browser sources | O4 | VideoToolbox GPU-direct + backpressure on POSIX | native | L | F1, P1 |
| Outputs, network ingest and browser sources | N1 | NDI output | native+shell | M | NDI decision |
| Outputs, network ingest and browser sources | I1 | SRT + RTMP ingest | native+shell | M | F1, P1 |
| Outputs, network ingest and browser sources | B1 | Browser host finished + bundled + overlay card | native+shell | L | P1 |
| Outputs, network ingest and browser sources | B2 | Preview staging + browser.* actions | shell+control | S | B1, Control |
| Comms, social review and remaining items | A1 | Comms libraries + Mac adapter port in the engine | native | M | none |
| Comms, social review and remaining items | A2 | Full engine glue, mic guard, send-threading spike | native | L | A1 |
| Comms, social review and remaining items | A3 | Shell parser + Comms model | shell | M | none |
| Comms, social review and remaining items | A4 | Comms page, leases, Private comms | shell | L | A3 |
| Comms, social review and remaining items | A5 | Live co-host acceptance | owner | S | A2, A4 |
| Comms, social review and remaining items | B2 | Review model + gated Show + sheet | shell | L | Metal, Graphics shell |
| Comms, social review and remaining items | B3 | YouTube chat | shell | M | B1, B2 |
| Comms, social review and remaining items | B4 | Twitch | shell | L | B1, B2 |
| Comms, social review and remaining items | R4 | Telemetry consent + events | shell | M | none |
| Comms, social review and remaining items | R5 | Crash upload | shell | M | R4 |
| Comms, social review and remaining items | R6 | Leftovers batch 1 | shell | M | none |
| Comms, social review and remaining items | R7 | Leftovers batch 2 | shell | L | none |

### Phase 5: Late and optional

Graphics Designer. VST3 hosting. NDI receive. Stream size independent of canvas. Dedicated Zoom ISO feed. WebM fallback.

**Outcome:** Full parity. Each item here has an owner decision on whether parity requires it.

| Workstream | Slice | What lands | Lane | Size | Depends on |
|---|---|---|---|---|---|
| Media clip playback | N5 | FFmpeg fallback for WebM | native | M | N1, decision |
| Metal graphics backend | G6 | Private graphics preview on Metal | native | M | G2, Grade G5 |
| Graphics in the shell | D1 | Designer lifecycle, layers, inspector | shell | L | G8 |
| Graphics in the shell | D2 | Designer canvas, snapping | shell | L | D1 |
| Graphics in the shell | D3 | Designer motion | shell | M | D1 |
| Graphics in the shell | D4 | Designer content, SVG | shell | M | D1 |
| Audio parity | V1 | Plug-in host load, probe and process on macOS | native | L | decision |
| Audio parity | V2 | POSIX transport + serve | native | L | V1 |
| Audio parity | V3 | Editor, parameters, state persistence | native+shell | L | V2 |
| Audio parity | V4 | Audio Units host (not planned) | native+shell | XL | decision |
| Outputs, network ingest and browser sources | N2 | NDI receive | native+shell | M | N1 |
| Outputs, network ingest and browser sources | B3 | ScreenCaptureKit capture upgrade (only if B1 is under 30 fps) | native | M | B1 |
| Outputs, network ingest and browser sources | O5 | Stream size independent of canvas | native+shell | M | O4, decision |
| Comms, social review and remaining items | R8 | Dedicated Zoom ISO feed (optional) | native | L | decision |

## 3. Lane order

- **Native:** Media N1, N2 → Grade G0 → Outputs O2 → Media N3, N4 → Grade G5, G6 → Sources S6 →
  Outputs F1, P1 → Metal G1-G5 → Tiles N1 → Comms A1, A2 → Outputs O3, O4, N1, I1, B1 → Audio A8 → Tiles N2 → late items.
- **Shell A (operator surfaces):** Sources S1, S2, S3, S5 → Media S1, S3, S2 → Grade G1-G4, G7 → Sources S4 →
  Graphics G1-G7 → Tiles S1-S5 → Graphics G8-G10 → Social B2-B4 → Designer.
- **Shell B (control, audio, settings):** Control RC1-RC4 → Audio A1, A2, A5 → Outputs Z1, Z2, O1 → Control RC5, RC6 →
  Audio A3, A4, A6 → Outputs D1 → Remaining R1-R3 → Comms A3, A4 → Audio A7, A9 → Control RC7 batches → Remaining R4-R7.

The native lane's long pole is the Metal graphics backend (about 5 weeks). Graphics shell slices G1-G3
can start against a scripted core, but nothing in graphics is proven live until Metal G2 lands.

## 4. Decisions that shape the schedule

| Decision | Recommendation | Effect |
|---|---|---|
| Ship a Mac release before the Graphics Designer exists? | Yes. Operate, data, crawls and Shot Layouts ship first; looks are authored on Windows until the Designer lands. | Removes 5-7 weeks from the path to a graphics-capable Mac release. |
| Does parity require VST3 plug-in hosting? | Not for the first parity release. Ship the audio console first, VST3 next. | Removes 5-6 native weeks from the main path. |
| How does the Mac get ffmpeg for streaming? | Bundle our own signed LGPL build. | Streaming, SRT, HLS and network ingest all wait on this. |
| Windows fonts that macOS does not have (Segoe UI and others) | A fixed substitution table, bundling Selawik after a licence check. | Blocks the template rasterizer (Metal G2). |
| Graphics or Tiles first? | Graphics Operate (G1-G7), then Tiles, then the builder and feeds. | Sets the order of Phase 3. |
| NDI on the Mac | Ship NDI output, defer NDI receive. | Moves one medium slice to the last phase. |

All 46 owner decisions, each with a recommendation, are in the specs' "Owner decisions needed" sections:

**Media clip playback**

1. WebM on Mac: Play it only when FFmpeg is installed; refuse clearly until then.
2. Chapter markers card: Do not port (it is a static placeholder on Windows).
3. Grade readouts on the Media tab: Keep the Mac grade window.
4. Local audition audio: Muted by default.
5. Zero-copy timing: After N1/N2; move it up if the N1 drill fails.

**Sources screen rebuild**

1. Thumbnail mechanism: A fixed core 'source sheet' IOSurface; fall back to a multiview crop if the drill fails.
2. Zoom slots after a meeting: Do not restore them; free on leave, as Windows does (Zoom reuses ids).
3. Auto-assign camera-off newcomers: Match Windows (assign them).
4. Role control: Hide it until role-targeted routes are ported.
5. Inline row controls: Move them into the inspector; keep a KEY chip on the row.

**Per-source colour grading and scopes**

1. Existing programme-wide grade window: Keep it as 'Program look'.
2. Grades after relaunch: Re-apply capture grades automatically; Zoom grades return as drafts.
3. Advanced grading before the private preview exists: Ship it with a warning.
4. Re-apply a grade after a Zoom stream change: Yes, keyed on the persistent id.

**Remote control parity**

1. Reply for a Windows action the Mac lacks: HTTP 422 with a plain reason; the manifest lists only what works.
2. OSC LAN opt-in: Identical to Windows (two environment variables, no token).
3. view.setMode: Leave blocked until Mac view modes are decided.
4. Read-only 'Remote control' status row in Settings: Yes.

**Metal graphics backend**

1. Windows fonts missing on macOS (Segoe UI, Bahnschrift, Cambria, Consolas): A fixed substitution table; bundle Selawik for Segoe UI after a licence check; warn, never fail.
2. Emoji draw in colour on Mac, monochrome on Windows: Accept the difference.
3. Ship G1 first on its own (slightly changes the lower-third look): Yes.
4. Size is about 5 weeks, not the roadmap's estimate: Accept.

**Graphics in the shell**

1. Ship a Mac release before the Designer: Yes: G1-G11 plus Windows show import.
2. Order against Tiles: G1-G7, then Tiles, then G8-G11.
3. Move Designer manifest-edit logic into the shared core: Decide at D1, leaning yes; not in this push.
4. Guest row titles before the Sources rebuild: Roster names with empty titles.

**CoreVideo Tiles**

1. Ship order: N1 + S1 + S2 as one releasable unit, then S3-S5, then N2/C1/S6.
2. Authored layers over the wall in the first Mac release: Defer (S6).
3. 'Name each tile' before the Metal raster fix: No; keep it disabled with a stated reason.
4. Opening Windows-authored Tiles show files: Out of scope; keep field names identical.

**Audio parity**

1. Does parity require VST3: No: ship A1-A9 first, V1-V3 next.
2. Audio Units as well: No.
3. Plug-in host entitlements: Library-validation exemption only; widen on evidence.
4. System audio on macOS 13.0-14.1: Process tap on 14.2 and later; a BlackHole-class device below.
5. Default Zoom audio topology: Keep per-guest ISO.

**Outputs, network ingest and browser sources**

1. ffmpeg on Mac: Bundle our own signed LGPL build (not Homebrew, not a native rewrite).
2. NDI in the first Mac release: Ship output, defer receive.
3. Browser page audio: Silent, matching Windows.
4. Stream smaller than canvas: Defer; ship 'follows the canvas'.
5. Order: Z1/Z2/O1/O2 now, F1 + P1 + D1 next, B1 after graphics.

**Comms, social review and remaining items**

1. Private comms default: Off, as on Windows.
2. When the Comms tab ships: Only after the live co-host test passes.
3. Social order: Keychain helper now; YouTube and Twitch last.
4. Crash reports cover: Shell, core and engine.
5. Dedicated Mac ISO feed: Defer.
6. Update banner: Link only, no in-app install.

## 5. What the specs corrected

Findings that contradict the 11-14 roadmap or the audits. Each was re-read in code by the spec author;
the first four and the Take item were also spot-checked while assembling this plan.

- The Mac already has a control server (HTTP, WebSocket, /snapshot, 13 actions). The roadmap and both 11-14 audits said it had no listener.
- Metal already has the Tiles rounded mask, border, glow and solid-fill paths. Roadmap item M5.1 shrinks from large to small.
- A WKWebView browser host already exists in the tree, unbundled and with an unfinished supervisor.
- SRT and HLS output are already compiled into the Mac release core; the gap is the shell UI and an ffmpeg built with SRT.
- The audit's 'basic per-source grade is shell-only' is wrong for Zoom sources: applying a grade needs an identity the Mac shell cannot get today (slice G0 fixes it).
- The audit's claim that ISO recording loses data without ISO buses is wrong: ISO stems come from source audio, not the buses.
- The VST3 host uses no JUCE or Steinberg SDK, so the Mac port is operating-system plumbing, not a licensing project.
- Take already goes through the transition command, and roadmap items M0.2 and M8.2 are already done on the Mac.
- The Windows action registry has 110 actions. The Companion module uses HTTP and WebSocket only, so OSC serves other rigs.
- Graphics in the shell is 13-17 weeks, far above the roadmap's sizing. The Metal backend is about 5 weeks.
- The shared media owner drops any frame without BGRA pixels, and the Metal compositor has no GPU-frame input, so a zero-copy decoder needs shared changes first.
- The roadmap had no audio milestone at all.

## 6. Top risks

- **Media clip playback:** BGRA clips cost two 8.3 MB copies per 1080p frame, one on the render thread. Not yet measured at 8 feeds.
- **Sources screen rebuild:** The thumbnail sheet renders on Program's render thread (Metal has no separate monitor worker). Gated by the 8-feed drill.
- **Per-source colour grading and scopes:** Preview and scopes share one GPU with Program. Only the 8-feed drill can clear it.
- **Remote control parity:** Network.framework UDP reply and multi-sender behaviour is unverified; spike first, BSD sockets as fallback.
- **Metal graphics backend:** Core Text widths differ from DirectWrite, so Windows-authored layouts may truncate or wrap differently.
- **Graphics in the shell:** Nothing can be proven live until the Metal backend exists.
- **CoreVideo Tiles:** Metal wall pixels have never been rendered or tested (the only decoration pixel test is Windows-only).
- **Audio parity:** Process-tap permission attribution from the helper core is unverified.
- **Outputs, network ingest and browser sources:** WKWebView snapshot capture may not reach 30 fps, and the alpha convention is unverified.
- **Comms, social review and remaining items:** Thread affinity of the Zoom SDK's channel audio send is unknown on macOS.

## 7. Rules carried into every implementation plan

- The 11-14 roadmap §6 rules stand: backlog first (one tracking issue per workstream, filed with owner approval),
  plans written to `docs/superpowers/plan-authoring-rules.md`, Windows rulings quoted with the test that proves them,
  a named Mac-twin check for every Windows-only `native/` module, and pixel claims proven with pixels.
- Every phase's native work includes a `scripts/mac-show-drill.py --load 8` run judged on per-frame delivery.
- A check that only simulates the system is not proof. The virtual camera lost a day to a sandbox profile that
  was verified by simulation and failed on the real launch (2026-11-15); system-level behaviour is proven on the installed build.
- Each spec's first slice starts by re-verifying its UNVERIFIED items.

## 8. Next steps

1. Owner installs installer 5 and confirms the virtual camera live.
2. Owner answers the six schedule decisions in §4 and reviews the Phase 1 specs (media, Sources, grading, control).
3. Write the Phase 1 implementation plans, one per workstream, and start the three lanes.
