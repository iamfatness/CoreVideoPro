# Audio Workflow Usability - VST, Buses, SETUP

_Owner ask (2026-07-12, verbatim): "I don't know how to add VST plugins, how to use
buses, the setup screen is clunky. Lots of work needed there." Status: SPEC - phases
U1-U4 below, none started. The DSP engine itself is done and owner-verified clean;
this spec is about the WORKFLOW wrapped around it._

## 0. The gap

The audio engine won its war (soak-verified clean, LV1-style console, 8-band EQ,
comp/gate with GR metering, mastering rack) - but the workflow assumes the reader
already knows the system's internals. Three symptoms, one cause: **the UI exposes
our implementation (inserts, chains, matrix cells, scan buttons) instead of the
operator's tasks (make my mic sound good, send the right mix to the right place).**

## 1. Audit findings

### VST plugins ("I don't know how to add them")
- The path exists but is invisible: processing panel -> Scan button -> browser list;
  or the rack strip's small "+ Add" flyout. Nothing says "this is how you get
  plugins in."
- Scan is MANUAL and the browser starts empty - a first-time user sees a dead pane.
- Status is honest but unexplained: built-ins show LIVE (green), third-party VSTs
  show amber "P2" - internal phase jargon. Nothing tells the operator that a
  third-party plugin currently passes audio through UNCHANGED.
- The deeper truth: third-party processing is only half-shipped. The host process
  exists (scan P1, probe P2a, bus-insert transport P2b with a -6dB test processor),
  but real plugin instantiation (P2c) is not done. On a CHANNEL a vst-named insert
  is a silent pass-through; on a BUS it currently applies the test processor. That
  split is indefensible UX and must be unified by P2c.

### Buses ("I don't know how to use them")
- The system has a real bus model (mon / pgm-l / pgm-r / stream / aux, master
  inherits to program) but it is NEVER EXPLAINED anywhere in the UI. The matrix
  shows ids, not purposes.
- Default routing (new sources auto-route to program + monitor + stream) is
  invisible - things work until the operator wonders WHY, then nothing answers.
- Rename/delete/create aux all exist (shipped) but are scattered; there is no one
  place that shows "here are your buses, this is what each is for, this is what
  feeds it, this is where it goes."

### SETUP screen ("clunky")
- The three-column reflow (Devices & master | Buses & matrix | Processing) organized
  the wall but kept it a wall: everything visible at once, no hierarchy of
  importance, no task flow. Device pickers, matrix, processing target, mastering,
  monitor controls compete at equal visual weight.
- No empty/first-run states: a fresh setup shows the same dense surface as a
  configured one.

## 2. Design principles

1. **One mental model, stated on screen: Source -> Channel -> Buses -> Outputs.**
   Every audio surface orients itself on that spine.
2. **Progressive disclosure.** The common case (one mic, default routing) needs
   zero SETUP visits. Depth appears when the operator asks for it.
3. **Truthful status, operator language.** No phase jargon ("P2"), no internal ids
   ("pgm-l"). If audio is unaffected, the UI says "not processing audio yet - passes
   through" in words.
4. **Same rules everywhere:** channel inserts and bus inserts behave identically
   (P2c is the prerequisite that makes that honest).

## 3. Phases (each independently shippable)

### U1 - VST workflow ("add a plugin and hear it in 30 seconds")
| Slice | What ships |
|---|---|
| U1a | Auto-scan on first plugin-browser open (+ background rescan button with last-scan time); browser gets search, vendor grouping, and per-plugin status in words: "Ready" / "Failed validation (reason)" / "Not processing yet - passes through" |
| U1b | One labeled entry point per strip: "Add plugin" button on the workspace RACK column and channel strip context menu (same list, same order: built-ins first, then VST3s) |
| U1c | Chip status legend + tooltips: green = processing live, amber = installed but audio passes through unchanged (until P2c); clicking an amber chip explains why + links the setting |
| U1d | (depends on VST P2c) third-party chips go green for real: channel + bus insert paths both route through the host; the -6dB test processor and the channel-side silent pass-through are DELETED |

Acceptance: from a cold app, the owner adds a VST to his mic channel and hears it
within 30 seconds without instructions.

### U2 - Bus comprehension ("what are my buses and what do they do")
| Slice | What ships |
|---|---|
| U2a | BUSES panel: one card per bus - human name + purpose line ("Monitor - what you hear in your headphones", "Program - what your audience hears / records / streams"), live meter, fed-by count, output destination; aux create/rename/delete move onto the cards |
| U2b | Matrix rehumanized: purpose tooltips on bus headers, "routed by default" badges on auto-created sends, an explainer line ("new sources route to Program + Monitor automatically") |
| U2c | Per-bus insert chain visible on the card (same chip UI as channels) - the bus is a first-class strip, not a matrix column |

Acceptance: the owner can explain mon/program/stream to someone else after reading
the cards, and creates + routes an aux without help.

### U3 - SETUP reflow round 2 (task-oriented)
| Slice | What ships |
|---|---|
| U3a | SETUP becomes four task groups with headers and one-line descriptions, in signal order: **Sound in** (devices, per-device options) -> **Mix & route** (matrix + buses) -> **Process** (channel/bus chains, plugins) -> **Monitor & master** (monitor device/volume, mastering, loudness target); groups collapse, remember state, and deep-link (an amber chip's "why" lands on Process) |
| U3b | Empty/first-run states: each group shows a setup prompt when unconfigured ("No microphone selected - choose one") instead of dense controls; configured groups show a one-line summary when collapsed |
| U3c | Density pass: consistent row heights/spacing, kill the giant device-name chips, align labels; screenshot-verify against the C-series console quality bar |

Acceptance: owner stops calling it clunky (screenshot-verify loop BEFORE shipping,
per the C-series lesson: four feedback rounds would have been two).

### U4 - Inline help
- "?" affordance per surface with 2-3 sentence explanations (the mental-model spine,
  what a bus is, what an insert is); no external docs dependency.

## 4. Dependencies and order

- **VST P2c is tracked separately** (real IComponent/IAudioProcessor hosting; needs
  a probe-passing plugin installed, e.g. free TDR Nova - WaveShells do not probe).
  U1a-c ship before it (honest amber); U1d ships with it.
- Suggested order: U1a-c (small, high-relief) -> U2a-b -> U3a-b -> P2c + U1d ->
  U2c -> U3c -> U4.
- Screenshot-verify (PrintWindow + UIA nav) EVERY visual slice before PR - standing
  rule from the console rework.

## 5. Explicit non-goals (this spec)

- No DSP changes (the engine is verified; this is workflow only).
- No plugin GUI hosting (VST editor windows are P4 in vst-host-spec.md).
- No ASIO, no per-source sync offsets (separate backlog items).
