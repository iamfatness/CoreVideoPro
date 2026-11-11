# Ported from ZComms

Source: ZComms (`iamfatness/ZComms`, MIT, same owner), `origin/main` @
`168a572d0f537a01a762ba97c8004ca7d7d0ff39`. Each file carries a banner naming
its origin. Re-port by copying again and re-applying the edits below; do not
hand-diverge.

## Verbatim (banner only)

| Here | ZComms |
|---|---|
| `talkback_sdk.{h,cpp}`, `talkback_sdk_win.{h,cpp}` | `src/zoom/` |
| `talkback_channels.{h,cpp}`, `talkback_eligibility.h` | `src/zoom/` |
| `reach.{h,cpp}`, `room_plan.{h,cpp}`, `duck_plan.{h,cpp}` | `src/zoom/` |
| `audio_defs.h`, `clock.h`, `devices.{h,cpp}` | `src/audio/` |
| `envelope.{h,cpp}`, `limiter.{h,cpp}`, `frame_ring.{h,cpp}` | `src/audio/` |
| `frame_accumulator.{h,cpp}`, `sample_ring.h`, `tx_pacer.{h,cpp}` | `src/audio/` |
| `signal_gate.{h,cpp}`, `miniaudio_impl.cpp` | `src/audio/` |
| `../../third_party/miniaudio/miniaudio.h` (v0.11.21, public domain / MIT-0) | `third_party/miniaudio/` |

## Edited

- `engine.{h,cpp}` (`src/audio/`): the echo canceller is compiled out unless
  `ZC_WITH_AEC` is defined (speexdsp and `aec.*` are not ported this slice).
  `SetAecEnabled` is a no-op and `aec_enabled()` false without it.

## Edge's own (not ported)

- `comms_core.{h,cpp}`: ZComms `src/app/main.cpp`'s orchestration (bank
  bring-up, auto-assign, healer, keys, duck, ops log) lifted out of the app
  into an SDK-free class shared by the real and fake engines, plus the
  `comms_state` JSON. Edits against the original are marked "Edge edit"
  (auto-assign marks a person only once a channel took them; auto-assign
  waits for the bank to settle).
- `comms_protocol.h`: the comms_* P2E line parser.
- `mic_guard.{h,cpp}`: the rule that the real microphone is never open to the
  room (when the virtual mic may be installed, ownership and the unmute only
  after `onMicInitialize`, the 5 s give-up, the immediate revoke on a drop,
  retry policy, the non-capture-only case). Pure, unit-tested in `native/tests/MicGuardTest.cpp`.

## Not ported

`aec`, `extern_feed`, `channel_mix`, `loopback`, `correlator`, `wav_sink`,
`generator`/`signal` (the chirp probe; TONE is the engine's built-in test
tone), `mic_source_win`/`virtual_mic` (the engine has its own never-fed
`SilentMic`), `roster` (polled in `engine-comms.cpp` instead: the engine's
participants controller event slot belongs to `EngineParticipants`),
`talkback_source` (single-channel spike), `breakout`, `chat_signals`,
`signal_*`, `zoom_client*`, `jwt`, the app/UI.

## Tests

ZComms' tests for the ported pieces are in `native/tests/comms/` (verbatim
plus banner; `test_util.h` maps `ZC_CHECK` onto Edge's harness).
