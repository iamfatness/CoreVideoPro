# Graphics Phase 3 Slice 2 — native crawl and safe publication

Owner request [#1098](https://github.com/iamfatness/CoreVideoPro/issues/1098). This narrows the [Phase 3 design](2026-11-03-graphics-phase3-data-production.md) to a crawl using the local CSV/JSON sources shipped in #1096.

## Operator workflow

In Design, create a Crawl from local data, choose a text column and see the first items and their order. Set the window and text style, direction, speed, item gap and playback mode. Save the design and add it to the playlist. Opening the saved design restores its binding and settings.

In Operate, Cue prepares a pinned list privately. Take starts the crawl on Program at its beginning, including when private preview has already played. Show starts an uncued row through the existing preparation path. The On Air row exposes Pause, Resume, Restart and Hide, plus the current revision, any pending revision and its status. A completed single pass keeps its row available for Restart or Hide. The operator can reload the local file, inspect the staged revision and use Publish data. Live follow is an explicit per-row setting. For local files it follows successful operator reloads.

## Geometry and timing

The core measures and prepares items once per revision with the existing text styles, font fallback and resource budget. Composite the prepared items through the fixed crawl window, intersected with inherited clipping. Measure actual advance widths. Apply the same clip and positions to Program and its alpha output. Render advances from the media tick in reference pixels per second. Rightward crawls mirror travel geometry while retaining item order and normal glyph orientation.

⚑ Defaults are leftward, 120 reference px/s, 48 reference px gap and Loop. Accept finite speeds of 1–1200 px/s and gaps of 0–512 px. Select at most 64 nonempty items in file order, expose any row limit in the authoring preview, and reject an empty result before cue. Reuse the existing text and prepared-resource limits. Persist settings and data bindings using the existing show and design lifecycle, with defaults for absent settings and empty crawl configuration on old shows.

⚑ Each pass brings the first item in from beyond the leading edge and ends after the last item clears the trailing edge. The next loop begins at that same boundary. Pause holds the exact distance and Resume excludes elapsed paused time. Restart explicitly resets distance using the accepted list. Duplicate Pause or Resume leaves playback unchanged. A completed single pass renders no crawl glyphs and retains its accepted revision. Hide follows the existing Out and retirement lifecycle even while paused or complete.

## Publication and failure behavior

Track the desired, prepared and accepted revisions for the active instance. Preparation and admission finish before a candidate is eligible. The newest valid request supersedes older pending work. Fence results by instance lifetime, request generation and template/data identity. A failed or stale result cannot mutate Program. Report failure while keeping the accepted content and playback state. Retire old resources through the existing lease path after the swap and fence late completions after removal or re-show.

⚑ Manual publication is default. Publish and enabled live follow queue a replacement for the next loop boundary on a running crawl. If the replacement arrives late, the old list loops until the next boundary. A paused crawl applies it only after Resume reaches a boundary. A single pass holds its original list, including across Restart of that instance. Store the selected publication separately from the staged revision so reopening a show does not put unpublished data on air. A new Show or Cue uses the selected published revision. When a tick crosses the boundary, apply any residual elapsed time to the new list. A swap preserves the instance, position anchor and In/Out state, and never exposes a partially prepared list. A queued private Cue remains pinned through Take.

## Acceptance

Use a synthetic local CSV and a saved show. Cue and privately play a crawl, Take it and confirm Program begins at the initial edge. Pause, stage and publish a different list, then resume and observe one complete switch at the loop boundary. Confirm one entrance, correct revision labels and working Hide. Repeat with live follow enabled, failed preparation, an overtaken revision and single-pass playback. Compare clipped pixels and alpha on frame-matched Program and NDI fill/key at 1080p. Capture preparation time, resident and retiring graphics bytes, GPU draw time and delivered frame continuity. Installed acceptance requires an isolated show and is recorded separately.

Remote polling, RSS, Google Sheets, push feeds, data-aware Shot Layouts and 4K capacity changes belong to later slices.
