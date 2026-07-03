# Changelog

All notable changes to CoreVideo Pro are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Nameable sources + auto-assigned Show Inputs.** Each Show Input now has an
  editable display name (the new NAME column in the Inputs tab) that defaults to
  the Zoom participant / UVC device / media asset name but can be overridden; the
  override feeds the **auto lower-thirds** and the multiview labels, and persists in
  `production-output-preferences.json` keyed by canonical source id
  (`zoom:`/`capture:`/`media:`). Separately, an **Auto-assign inputs** automation
  toggle (default on) fills FREE Show Input slots with newly-joined Zoom
  participants without disturbing operator- or capture-assigned slots, keeping each
  participant in a stable slot and freeing slots when participants leave.

- **Overlay / lower-third / caption rasterization (Item 9)**: overlays now render
  real content instead of colored rects. A shared layout resolver
  (`OverlayTileRaster::computeOverlayTileLayout`) defines the band, accent bar,
  image slot, and text-line geometry once; the portable CPU preview rasters it
  with a full-ASCII 5x7 bitmap-font tile, and the Windows D3D11 compositor
  rasters the same geometry with DirectWrite/D2D antialiased text (brand font
  family) plus a real WIC `imageUri` decode, rendered zero-copy into a GPU
  texture via a D2D DXGI-surface render target. Rasters are cached by content
  signature and re-run only when content changes — keyPhase animation stays a
  composite-time transform. Caption speaker attribution now uses the secondary
  brand accent so it reads distinctly from the accent bar.

- **GPU core-composited multiview**: the C++ core composites the whole multiview grid
  into ONE keyed-mutex DXGI shared texture (replacing per-tile swap chains and CPU
  tiles), presented by a single WinUI surface with overlay labels, red/green tally,
  audio meters, and a clock; four operator-selectable layout modes (grid,
  pgm/pvw top, pgm/pvw large, pgm/pvw side) with aspect-correct 16:9 tiles.
- True multi-layer **PREVIEW composite bus** (core + WinUI + multiviewer).
- **Phase 2 threading decouple**: audio mix/route, monitor render, BS.1770 loudness,
  encoder submit, and output-sender sync moved off the render/command threads onto a
  dedicated ~50Hz worker with a two-lock (`coreMutex`/`audioOutputMutex_`) discipline;
  empty `media-core-sync` polls no longer run a heavy tick. Render thread is
  video-only, targeting locked 60fps.
- **Phase 2 increments 3 + 6 (threading leftovers)**: Zoom engine stdin writes moved
  off `coreMutex` onto `ZoomEngineRuntime`'s outbound FIFO queue + dedicated sender
  thread (ordering preserved; subscription dedup keyed at enqueue; engine
  restart/shutdown drop queued lines for the dead process and log — a wedged engine
  pipe can no longer stall the command/spine path), plus a `coreMutex` hold-duration
  guardrail (`core/LockHoldGuardrail`) enforcing the sub-ms hold contract with
  per-site telemetry and rate-capped warnings (opt-in strict abort via
  `COREVIDEO_LOCK_GUARDRAIL_STRICT=1`); sanctioned long-hold sites carry their own
  budgets. Lock order is now `coreMutex → audioOutputMutex_` and
  `coreMutex → ZoomEngineRuntime::mutex_ → ::sendMutex_`.
- **60fps pipeline**: zero-copy Zoom I420 ingest and a precise render-loop pacer.
- Preview/program parity fixes: per-participant color grade applied to source exports
  (brightness parity) and preview-freeze fixes when a source is in both preview and
  program.
- Native Studio **Routing tab** with an audio crosspoint gain matrix: assigned
  Inputs (1–10) plus the Zoom program mix and media as rows, program/ISO/monitor/
  stream buses as columns, and per-crosspoint on/off + gain (dB) editing, wired to
  the native core via `sync-audio-routing-matrix`.
- Real program-audio pipeline in the C++ media core: PCM routing matrix with
  program/ISO taps, WASAPI monitor output (dev-gated), a BS.1770 master loudness
  meter on the program tap, and per-bus insert dynamics (compressor + limiter).
- Recording now muxes the real composed program frame plus real program audio and
  honors the requested resolution/fps from the recording command.
- RTMP sender feeds the real program-audio tap (falling back to silence only when
  no PCM is available that tick) and resolves an H.264/AAC codec-compatibility
  matrix before encoding.

### Fixed

- **Recording/encoder overload resilience: the audio worker no longer collapses
  and stop-recording can't be starved under load.** Under a worst-case soak the
  audio/output worker fell from 47 to 0.6 ticks/s and the operator couldn't stop
  recording for minutes. Root cause: the worker called `encoder->submit`/
  `submitAudio` synchronously while holding `audioOutputMutex_`, so a blocking
  Media Foundation `WriteSample` (disk stall) wedged the worker — and stop-recording
  needs that same lock to finalize the container. The new `AsyncEncoderSink`
  decorator (wired in for the live server only, via `createLiveServerModules`)
  drains the encoder onto a dedicated writer thread: `submit`/`submitAudio` are
  non-blocking enqueues with a **drop-to-latest** backlog cap (recording degrades to
  a lower fps instead of collapsing), `stopRecording` returns instantly (finalize
  runs async), and teardown finalizes within a **bounded grace** (then detaches so
  shutdown never hangs). Separately, the `JsonRpcServer` command loop now
  **coalesces stale `media-core-sync` batches** — a sync that already has a newer one
  queued behind it is answered from the current snapshot without the expensive
  apply/render pass, so a command backlog can't delay the level-triggered
  stop-recording (in the newest sync) by minutes. Every request still gets a
  response.
- **Zoom engine IPC no longer collides with the OBS zoom plugin.** The engine's
  named pipes, unix sockets, and shared-memory regions previously used fixed
  names on the shared `ZoomObsPlugin_` base, so whenever OBS (with
  `obs-zoom-plugin`) was running, every join failed with "Timed out connecting to
  Zoom engine IPC." The parent (`ZoomEngineProcessClient`) now generates a
  per-instance token (`<pid>-<spawn#>`), passes it to the engine via
  `--ipc-token`, and both sides splice it into every IPC name
  (`ipc_pipe_p2e`/`ipc_sock_p2e`/`ipc_shm_prefix` in `engine-ipc.h`). This also
  lets two CoreVideo Pro instances run side by side without corrupting each
  other's video. Real and fake engines both honor the token.

### Known gaps

- SRT **output** is not yet implemented; live UVC/DeckLink/AJA frames
  do not yet reach the core. See `docs/native-production-completion-plan.md`.
  (Overlay / lower-third / caption **text rasterization** shipped 2026-07-02: the
  D3D11 compositor rasters real DirectWrite text + WIC images via a D2D
  DXGI-surface render target, cached by content signature; validated on-GPU by
  pixel tests and live in the app at 60fps. The portable CPU/preview path
  rasters the same `computeOverlayTileLayout` geometry with the full-ASCII
  bitmap-font tile, so preview and program agree by construction.)
- The **alpha validation pass** (`docs/alpha-plan.md` Tracks A–F) has not been
  executed: no live-Zoom proof, record/stream soak, or clean-machine packaging
  evidence yet — including the ≥10-minute audio-glitch-freedom soak for the Phase 2
  threading decouple.
- No production code signing / notarization or auto-update channel yet.

## [0.1.0] - 2026-06-15

### Added

- Native desktop architecture: **WinUI 3 (.NET 9)** shell embedding the React/Vite
  operator UI, talking to a **C++ media core** over a typed JSON-line
  command/snapshot protocol, with a Node.js mirror for in-container parity.
- CI gates: segmented vitest (unit + integration), Node media-core tests, C++
  media-core stub build with a contract-parity gate, and a Windows WinUI publish
  gate.
- Windows packaging via the WinUI shell (`npm run pack:native`,
  `npm run pack:native:msix`) with optional local dev signing.
