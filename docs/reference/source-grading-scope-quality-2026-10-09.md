# Source grading scope quality — 2026-10-09

Owner finding for #835: the waveform looks compressed and the graphs look low resolution.

The review page stretched fixed-size canvas buffers to the panel size. Its canvases now use actual CSS dimensions times device pixel ratio, with redraw on resize. The waveform receives twice the width of the adjacent scopes, a taller plot and a larger expanded view. Axes reserve their own space; the vectorscope preserves square geometry. This HTML remains a demonstration, not a live meeting monitor.

The native analysis used 64×64 waveform/channel and vectorscope bins. These now use 256×256 bins, and the scope export increases from 768×256 to 1536×512. The scope host increases from 210 to 300 pixels high. Both managed texture validation layers require the new size. Source sampling remains 256×144 on the existing bounded private worker; the higher display/bin resolution does not imply full-resolution source analysis. The bin buffer increases to 328,708 uints and the export has four times the pixels, so sustained performance qualification remains necessary.

Local evidence in `health-reporting-765/artifacts/`:

- `scope-quality-build.log`: native Release build passed.
- `scope-quality-pixel-tests.log`: six GradePreviewPixels tests passed, including independently predicted neutral, RGB and skin-patch histogram/waveform/vectorscope locations.
- `scope-quality-shell-build-v3.log`: final WinUI Release build passed.
- `scope-quality-protocol-tests.log`: six protocol tests passed, including accepting the new size and rejecting the old size.
- `scope-quality-editor-tests.log`: eleven focused editor tests passed.
- `scope-quality-workspace.json` and `scope-quality-workspace-v2.json`: failed real-XAML probes retained. Updating only the view-model size check was insufficient because the protocol parser also rejected the new export size.
- `scope-quality-workspace-v3.json`: final real-XAML probe passed. Both bound GPU surface hosts opened, scope expansion and original taps worked, and Basic/Advanced plus resize layout passed.
- `scope-quality-workspace-v3-layout.png`: offscreen XAML layout only; SwapChainPanel pixels are excluded by RenderTargetBitmap.
- `scope-quality-review-full.png`: browser rendering of the expanded waveform; display-sized backing canvas verified at 500×360 for a 500×360 CSS canvas on the current 1× display.

The installed beta has not been replaced by this change. These checks do not establish physical-display acceptance, sustained 1080p60 delivery or fleet coverage. Earlier per-frame qualification limits remain open.
