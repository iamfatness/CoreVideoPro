# Browser sources (BR-1, 2026-07-13 — render-only URL sources)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`docs/capture-sources-spec.md` §4 status block has the full shape. The short version:

- **One `corevideo-browser-host.exe` per source** (`native/browser-host/`, the
  zoom-engine isolation pattern — page content never runs in the core). It renders the
  URL in a windowed WebView2 controller inside a borderless WS_EX_TOOLWINDOW positioned
  OFF the virtual desktop, **WGC-self-captures its own window**, and publishes BGRA into
  a `Local\CoreVideoPro.browser.<pid>.<n>` seqlock SHM (`BrowserSourceShm.h` — same
  layout as the WinUI capture bridge). **Real alpha survives** (rig-verified: transparent
  page regions arrive premultiplied alpha=0), so graphics tools key over program.
- **Two non-obvious host requirements:** Chromium marks offscreen windows occluded and
  throttles rAF to ~1fps — the host passes
  `--disable-features=CalculateNativeWinOcclusion` (+ background-throttling off), which
  is what makes offscreen rendering sustain ~28fps; and the window must be per-monitor
  DPI-aware with `RasterizationScale` pinned to 1.0 or CSS pixels ≠ frame pixels.
- **Core side:** `BrowserSourceHostAdapter` (owned directly by MediaCore, not in
  ModuleSet) — commands `browser-add {url,width,height,fps}` / `browser-remove` /
  `browser-reload {browserId}`; frames keyed `capture:browser:<n>` merge into the
  capture stream, sources enumerate as capture devices (vendor "browser"), health in the
  snapshot `browserSources` node. Supervision mirrors `CaptureReaderStallPolicy`:
  host death → LOUD stderr + last frame held 2 s → slate, respawn with 5→10→20→40→60 s
  backoff, **give up after 5 consecutive failures** (operator reload resets). Spawns run
  on the adapter's supervisor thread, never under `coreMutex`. URLs are validated (no
  quotes/whitespace/control chars; http(s)/file/data only) because they ride a
  CreateProcessA command line. Host stdin is the control pipe: `reload\n`, EOF = quit
  (no orphan hosts).
- **Shell:** Sources tab "Add browser source" (URL + preset), Browser group in the
  unified picker (`ShowInputKind.Browser` is capture-class everywhere), control API
  `browser.add` / `browser.remove` / `browser.reload`.
- **WebView2 SDK is VENDORED** at `third_party/webview2/` (NuGet 1.0.3800.47, BSD-style
  license, static loader — provenance in its README). The evergreen **Runtime** is
  probed at host startup and missing-runtime fails loudly (exit 3).
- **Self-test / render proof without the app:** run the host standalone —
  `corevideo-browser-host.exe --url <url> --width 640 --height 360 --fps 30
  --dump-bmp out.bmp --dump-after-ms 8000 --exit-after-ms 10000` prints machine-checkable
  pixel stats (mean BGRA + samples) and a capture-fps line every 5 s.
- **Known BR-1 limits:** per-frame CPU copy on the render tick (same cost as one bridge
  capture device; BR-1.5 = keyed-mutex shared texture), no page audio (BR-3), no
  interactivity/zoom/custom CSS (BR-2), navigation is unrestricted (popups/downloads are
  blocked). Static pages deliver ~0 fps by design (WGC fires on change; the core
  re-serves the held frame).
- **Build gotcha honored:** the new exe is in BOTH the cmake `--target` list AND the
  staging list in `scripts/build-native-dev.ps1`. Same-change fix: that script no longer
  aborts after a FRESH zoom-SDK stage (`$LASTEXITCODE` was null → treated as failure).
