# CPU preparation production integration (#802)

The owner authorized Windows production defaults and test-meeting qualification
on October 11. Normal module composition now enables the bounded preparation
owner and completed-image admission, with optional monitor/participant exports
on an isolated compositor. Explicit environment overrides remain available for
comparison; GPU capture ingress and the configured two-frame Program buffer are
unchanged.

The installed incident showed cold 1920×1080 background texture creation plus
BGRA upload work exceeding the 16.67 ms Program deadline. An expired-ready queue
head then amplified the original loss. The queue now retires that head before
waiting for the same following slot. It still records the original missed slot.

## Local validation

- Release native core and test executable built successfully.
- Eight hardware Program-buffer tests passed, including actual NV12 identities
  following an injected late preparation. Reverting only the queue repair makes
  that regression fail; restoring it passes.
- Ten I420/source-preparation tests passed, including normal factory defaults,
  actual pixels, faults, and lease behavior.
- Full native suite: 1,566 passed, two webcam-framer failures. Both failures also
  reproduced in the unchanged `idle-recording-state-859` baseline executable;
  they are not waived or counted as passing.
- Candidate native SHA-256:
  `f7ddf77cebe818973344d4f65b362bdafdad8adb20cc4632f755121b6b4a9399`.

## Live qualification

`scripts/qa/media-background-admission-soak.mjs` uses the real SDK helper,
requires frames from every selected camera, repeatedly opens the supplied
1920×1080 background under new asset identities, and checks delivery counters
without changing buffer depth. An independent registered OS camera receiver
reads pixel identities; its verdict is separate from publication counters.
Credentials are supplied through the child environment and excluded from
committed evidence.

The signed-in candidate joined, but the SDK reported
`CanStartRawRecording = 12` and requested recording privilege from the host.
Raw video remained unavailable while the host grant was pending. **Zero misses
has not been validated.** Empty-scene counters, successful joining, or average
frame rate do not satisfy that requirement. The live run must finish with all
feeds, every cold background admitted, no missed delivery slots, and a passing
independent receiver before this can be accepted. Physical display presentation,
CPU ISO continuity, unsupported adapters, and fleet behavior remain separate
qualification boundaries.

Private ignored logs and before/after test evidence are in
`artifacts/qa-802/` on the reference machine.
