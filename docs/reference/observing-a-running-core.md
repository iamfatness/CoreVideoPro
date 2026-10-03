# Observing a RUNNING core: `GET /snapshot` (2026-09-09)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`GET http://127.0.0.1:8011/snapshot` serves **the core's own sessionState JSON**, verbatim,
from the snapshot the shell already holds. It exists because `ControlState` forwards only a
handful of hand-picked `Native*` fields and the typed `NativeMediaCoreStateSnapshot` binds only
what the shell consumes — encoder evidence, real-time worker evidence, the program buffer,
tiles, multiviewer, browser sources and ~40 other nodes were parsed and dropped. A qualification
judge can now watch the core the operator is actually running instead of spawning its own.

- **How it flows.** `CoreProtocolParser` / `MediaCoreSupervisor` tag every parsed snapshot with
  `RawJson` + `RawReceivedUtc` (`[JsonIgnore]`, in-process only). **Both** sync paths must tag:
  a real core answers with a WIRE state that is mapped onto a neutral base
  (`NativeMediaCoreStateMapper`), so tagging only `TryParseSyncSnapshot` leaves the live path
  with no raw at all — that is exactly the bug this endpoint was first caught by.
  `StudioControlSurface` (an `INativeSnapshotObserver`) reads the reference the bridge already
  publishes: no core round-trip, no UI marshal, no render-path lock.
- **Envelope.** Always states `available`, `receivedUtc` (shell receipt), `servedUtc`, `ageMs`
  and `stale` (>2s). Absent / synthesized / unparseable snapshots answer `available:false` with
  a `reasonCode`, never something that reads as current.
- **Auth.** Same rules as every other route — loopback needs none, a LAN bind (`"+"`) refuses to
  start without `COREVIDEO_CONTROL_TOKEN`. No new unauthenticated surface.
- **Redaction** (`CoreSnapshotObserver`, tests in `CoreSnapshotObserverTests`): applied at the
  observation boundary, not per tick. It **walks the JSON** and filters each string value through
  `SupportBundleLogRedactor` + drops secret-NAMED values. Do not run that redactor over the
  document as text: its rtmp rule is greedy over non-whitespace, so one URL in a `lastError`
  eats the closing quote and the properties after it. Name matching is by suffix
  (`…Key/Token/Secret/Password/Passphrase/Jwt/Zak`) so `keyPhase`/`keyer`/`keyPosition` survive.
  Audit result: the snapshot carries **no** stream keys or passphrases (destination settings are
  inputs; the RTMP/SRT adapters already publish `redactedEndpoint`), but it does carry adapter
  free text that could quote one, operator browser-source URLs, and recording paths — paths are
  deliberately kept, matching the support bundle's "ISO paths are not secrets".
- **Per-layer geometry does not exist on the wire.** `RenderedProgramSources.h` publishes exactly
  `layerId/sourceId/participantId/kind`; rect / fit / opacity / fill colour live on
  `CompositorRenderPlanLayer` inside the core and are never serialized. `ControlProgramVideoSource`
  adds `order` (the index in the core's already-sorted publish order). Only the `tiles` node
  carries a rect per member. Adding real geometry is a **core** change.
- **The mapped snapshot says only what the core said (#740).** `NativeMediaCoreStateMapper`
  starts from `new NativeMediaCoreStateSnapshot()` (idle, no recording, no senders, no preview,
  no meeting) and overlays the core's JSON. It used to start from `SyntheticMediaCore`, a
  snapshot derived from the COMMANDS the shell had just sent, so a node the core omitted read
  as the outcome the shell had asked for: a `start-recording-session` in the batch produced
  `Active/recording/writing/HardwareAccelerated`, a `start-program-output` produced a live
  RTMP sender at 6 Mbps and a generated gradient as the Program preview. The mapper no longer
  takes the commands at all — do not add them back. Against a current core only `recording`
  (omitted until the first session) and `programFramePreview` (omitted whenever the buffered
  GPU path skips the CPU readback) are ever absent; both now map to null. A missing or
  unrecognised `health.programFrameHealth` maps to `unknown`, not `live`. `outputProfile` is
  bound from the core; it used to be a hard-coded 1080p60 / 8 Mbps.
  Still not from the core: `SourceSnapshot` (idle from the mapper; the Zoom capture and spine
  mergers fill it from the roster, and count participants as live frames), `RenderPlan`
  (layers and routes always empty), `OperatorActions`, `EventLog`, `Frames`. Read those nodes
  from `RawJson` (`sources`, `zoom`, `tiles`) if you need the truth.
- **Media frame-delivery trace (#701, 2026-10-03).** Each `mediaSources[]` row carries
  `decodedVideoFrames` / `lastDecodedAgeMs` (the transport worker pushed a decoded frame),
  `presentedVideoFrames` / `lastPresentedAgeMs` (the render tick put a NEW frame on air),
  `videoQueued` (prepared frames waiting) and `decoderRestarts`. A frozen clip is one of
  three things, and the row says which: decoder stalled (decode age grows, queue drains),
  presentation stalled (frames decoded or queued, presented age grows), or Program itself
  (read `programBuffer.deadlineMisses`/`underruns` for the same window). A paused clip holds
  its on-air frame, so its presented count stops by design. `python
  scripts/qa/media-delivery-trace.py` samples the control API and names the seam when a live,
  on-Program source's frame has not changed for `--stall-ms` (default 1000).
