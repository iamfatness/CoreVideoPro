# Control-state crossing inventory

Linked issue: [#661](https://github.com/iamfatness/CoreVideoPro/issues/661).
This inventory records the current Windows path for the first revisioned
command. The owner and recovery rules are in
[control-state-events-spec.md](control-state-events-spec.md).

| Crossing | Producer and owner | Consumer and current trigger | Current test / gap |
| --- | --- | --- | --- |
| Zoom mute, talking, roster | Zoom SDK callbacks in `EngineParticipants`; Zoom is the fact owner | Native `ZoomEngineRuntimeState` and core snapshot, then `ZoomCaptureSnapshotMerger` / `LiveProductionSync` into strip and Sources | #659 adds meeting/revision snapshot barrier and replay; installed transition still qualifies it |
| Operator strip mute | Shell control document | `MediaCoreCommandBuilder.BuildAudioMixCommand` in repeating production sync; core mixer applies it | #608 separates it from Zoom mute; no revisioned multi-client conflict yet |
| Audio routing matrix | Shell control document | `BuildAudioRoutingMatrixCommand` in repeating production sync; core `syncAudioRoutingMatrix` | Core has hold-last for transient empty matrix; command acceptance and draft/applied identity are not revisioned |
| Audio monitor enable, device, volume | Shell preferences; Control API `audio.monitor.set` / `audio.monitor.volume` mutate the same VM properties | `OnAudioMonitorSettingsChanged` saves, then repeating `BuildAudioMonitorCommand` invokes core `syncAudioMonitor`; core snapshot reports `audioMixSession.monitor*` | There is an applied readback, but no epoch/revision, operation ID, or stale-client rejection; this is the bounded #661 domain |
| Preview scene and show-input assignments | Shell scene document; Control API `input.assign` mutates editor properties | `BuildPreviewSceneCommand` and scene graph batch; core publishes Preview/Program | No accepted versus applied revision; #661 does not rewrite Take |
| Recording/stream request | Shell/Control API intent | Native output adapters own session state and progress; snapshot reports result | Desired booleans and real output progress are separate but not a general command conflict contract |
| Core session snapshot | Native core | `MediaCoreBridgeService` polls ~250 ms and publishes immutable read model; spine sync merges subscription evidence | #621 observes state and #622 bounds commands; they do not prove a stale edit is rejected |

For the monitor domain, the shell's property values are a draft. The native
`audioMixSession.monitorEnabled`, device and volume are applied values; status,
frames and underruns are observations. The Control API is a second command
client, not another owner. A full sync must not silently reapply an old monitor
draft after a newer client command is accepted.
