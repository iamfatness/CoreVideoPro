# Graphics Phase 3 — advanced data and production graphics

Status: proposed implementation scope. The original [titler roadmap](../../titler-service-spec.md#implementation-phases) owns product direction. This document scopes Phase 3 against the Windows beta that shipped Designer Slices 1–5. The [backlog](../../BACKLOG.md) owns scheduling. Phase 2 installed acceptance remains an independent check.

## Operator outcomes

- Import a data file, map its columns to a table or leaderboard, and use that graphic in Operate. Change the data and decide when the next revision reaches Program.
- Run a crawl with configurable speed and direction. Cue it privately, start it on Take, and hide it from the same Operate surface while it runs.
- Connect a remote data source and see the last successful refresh, freshness, and errors. Keep the current Program graphic usable through a feed outage.
- Cue a shot layout with explicit data values or a data revision. One Take carries the scene and prepared graphics together.
- Use HTML as an opt-in graphics source for designs that need it. Keep its controls and failures separate from existing native designs.
- Verify equivalent graphic content on Program and NDI fill+key. Qualify higher density and 4K using measured frame and resource evidence.

## Model and revision ownership

A show stores **data source definitions**, **bindings** and **publication settings**. The shell owns parsing, fetch scheduling, credentials and typed validation. Each successful import or refresh produces an immutable dataset revision with a stable source ID, field IDs, source revision, receipt timestamp and keyed rows. A publication record references the selected revision. Existing Sources and names-list behavior remains available through its current paths.

Column types are text, number and boolean in the first table slice. Colours and image assets can use existing Designer field validation when explicitly supported. Missing data uses an explicit fallback defined by the binding. Invalid data, duplicate row keys, oversized batches and ambiguous headings reject the whole new revision. Stable row keys are required for live feeds and leaderboards. File imports can generate deterministic row IDs from their original order when no key is selected. Leaders sort by a typed numeric field with a stable row-key tie-break. The operator can see the sort and row limit before cueing.

Native preparation receives a bounded resolved value document and its revision identity. Native owns layout, measurement, glyph preparation, composition, and timing. It performs no file or network reads for datasets on the render path. The existing template pin, cue revision and graphics instance identity remain distinct from data identity. A cue captures a specific data revision. An out-of-order preparation or fetch cannot replace a newer accepted revision. Show documents created before Phase 3 load with empty data configuration.

⚑ **Starting limits.** A dataset has at most 1,000 rows, 32 columns and 1 MiB of decoded data. An on-air table has at most 12 visible rows and 8 columns. A crawl prepares at most 64 items. These are implementation defaults for initial qualification. Existing layer, field and per-bus caps remain in force until evidence supports a separate increase.

## Native data graphics

**Table and leaderboard.** Add a data-driven graphic component with a header, repeated row styling, column widths and alignment, and existing text overflow behavior. Native preparation uses shared styles and a bounded raster budget. Data-only updates prepare a new revision off the render path and swap complete pixels when ready. A leaderboard is a table with stable numeric ordering and a displayed rank.

**Crawl.** A bounded ordered list of items renders through a clipped window. The operator sets speed in reference pixels per second, left or right direction, gap and single-pass or loop behavior. Native uses the existing media tick and a fixed start anchor. Pause freezes the current offset, Resume continues it, and restarting is explicit. Cue and private preview have independent clocks. Hide remains available while paused or playing.

⚑ **Publication policy.** Manual publication is the default. Explicit live follow may advance the content of an on-air data graphic at a safe boundary. A ready table swaps on a frame boundary. A looping crawl adopts the new list at the loop boundary. A single-pass crawl retains its pinned data until a new Show. A content update preserves the instance and does not restart In motion. Failed refresh or preparation keeps last accepted Program pixels and shows stale or failed status in Operate.

## Data adapters

1. Start with local CSV and JSON files selected explicitly. Reload is an operator action. Provide a preview with row count, types and validation errors before publishing.
2. Add HTTPS polling for Google Sheets and RSS, with REST/JSON using the same bounded transport and typed normalizer. Store credentials through the existing protected credential facility selected during implementation. Show documents and diagnostics may carry a credential reference and redacted status.
3. Add WebSocket and social feeds through the same adapter contract after pull sources work end to end. Normalize messages into bounded batches, validate ordering or session resets, and discard superseded work. Each provider has its own capability and acceptance gate. Sports adapters remain Phase 4.

⚑ **Network defaults.** Remote sources are off until enabled by the operator. Use a 15-second polling default with a 5-second minimum, one in-flight request per source, bounded response bodies, cancellation, and capped backoff. Treat configured URLs and redirects as untrusted. Restrict local and private destinations by default, including after DNS resolution and on every redirect. Rehearsal uses loopback through a test-only transport. Never emit credentials, query strings or raw feed contents in support logs.

## Shot layouts and macros

Extend the shipped Shot Layout surface with typed row-value overrides and data revision selection. Resolve scene, rows, field schema, source freshness policy and capacity before the first side effect. Stage the resolved inputs through the existing cue path. Take is enabled after every required cue is ready. A failed preflight or preparation keeps the old Program state and identifies the failing item. Keep a visible Cancel action. Repeat Take reconciles the existing operation outcome and does not replay a successful mutation.

⚑ **First macro boundary.** The initial implementation uses existing scene and graphics actions plus typed data overrides. Audio actions require a separate revisioned command and explicit readiness/rollback contract before being added. Free-form scripts and arbitrary API calls are outside this macro surface.

## HTML, output and performance

HTML uses a separate, opt-in template family and the existing browser-source architecture as a candidate transport. The qualification spike must prove process isolation, transparency, frame identity, bounded preparation, and failure recovery. Treat authored HTML as untrusted. Start with packaged local assets, no host APIs, local-file access beyond the package, credential sharing or default network access. A failing or hung HTML process leaves the last accepted Program resource in place or follows the explicitly configured fallback. Native declarative templates remain the normal authoring path.

NDI fill+key uses the same accepted native graphic revisions as Program and the current straight-alpha, frame-matched output path. Check transparent and opaque pixels and pair timing at the receiver. SDI key/fill is a separate device qualification task with hardware-specific evidence. It does not delay the existing NDI path.

⚑ **Performance gate.** Use current 1080p delivery as the regression baseline. Measure preparation time, resident and retiring graphics bytes, skipped refreshes, stale age, GPU draw time and actual output frame continuity. Measure 4K on supported hardware before raising any limits or claiming 4K performance. Use the existing output gate and its zero-missed-frame requirement for the full workload. Report each resolution and hardware class verified.

## Acceptance

- Import keyed CSV, show a leaderboard from Operate, publish a reordered result, and confirm the next accepted table frame contains a complete revision with stable tie order.
- Cue a crawl privately, take it, pause and resume it, and publish the next revision at the selected boundary. Confirm no motion restart, no Program changes from private play, and a working Hide.
- Disconnect a feed while a graphic is live. Confirm its last accepted values and stale status, then recover without briefly showing an obsolete response.
- Cue a layout with two graphics and overrides. Confirm one Take carries the correct values and scene. Repeat with one unavailable revision and confirm Program holds.
- Verify Program and NDI key pixels and frame identity at 1080p. Run the measured 4K workload before claiming higher density or resolution. Verify HTML and SDI separately if those tracks proceed.

Phase 4 remains the home for sports modules, shared libraries, collaboration, AI assistance and independent channels. PSD import, general keyframe authoring and other items deferred from Phase 2 require their own scope decisions.
