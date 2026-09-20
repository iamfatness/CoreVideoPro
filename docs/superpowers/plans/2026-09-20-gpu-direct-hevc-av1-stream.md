# GPU-direct HEVC and AV1 for the live stream — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The live stream encodes HEVC or AV1 on the GPU-direct path (hardware MFT fed from the compositor texture, FFmpeg as a copy muxer over enhanced RTMP) at 1080p60 real time, and a codec the machine cannot honor refuses the stream start with a named reason instead of silently sending H.264 on the slow raw path.

**Architecture:** The codec rides the existing `GpuVideoEncoder` seam as one string field; the Media Foundation implementation binds the hardware MFT for that codec (HEVC with B-frames off, because the FLV muxer refuses reordered raw HEVC); the FFmpeg bitstream mode names the raw demuxer per codec; a new pure `StreamStartAdmission` policy decides refusal codes from the compatibility matrix, the capacity probe and the chosen path, and the sender applies it once at start. Everything else in the pipeline (dedicated encoder device/thread, bitstream writer, supervisor, send proof) is unchanged.

**Tech Stack:** C++20 core (`native/`), Media Foundation hardware MFTs (`MFTEnumEx`, `ICodecAPI`), FFmpeg N-124549 LGPL as muxer, WinUI 3 / .NET 9 shell for the operator sentence, in-house gtest shim (`native/tests/gtest/gtest.h` — `--gtest_filter` takes ONE positive wildcard, no `:` lists, no `-` negation), Node gate script.

**Spec:** `docs/superpowers/specs/2026-09-20-gpu-direct-hevc-av1-stream-design.md`

## Global Constraints

- Codec strings on the seam are normalized to exactly `"h264"`, `"hevc"`, `"av1"` (`"h265"` → `"hevc"`); `RtmpCompatibility` keeps its own `"h265"` spelling at the operator-settings boundary and the sender maps between them.
- HEVC on GPU-direct sets `CODECAPI_AVEncMPVDefaultBPictureCount = 0`; a MFT that refuses it fails `start()` (spec §2).
- Raw demuxers in bitstream mode: `-f h264`, `-f hevc`, `-f obu`; `-use_wallclock_as_timestamps 1 -r <fps>` and `-c:v copy -f flv` unchanged; no `-tag:v` (spec §3).
- The retired reason `"codec-not-h264"` must not survive anywhere; a codec without a GPU encoder reports `"no-hardware-encoder"` (spec §4).
- New `lastResultCode` values, exactly: `enhanced-rtmp-required`, `no-hardware-encoder`, `gpu-encoder-start-failed` (spec §5, §7).
- `EncoderPolicy.h` comment must carry the dated reversal: "2026-08-06 HEVC exclusion reversed by the owner on 2026-09-20; patent exposure accepted" (spec §5).
- Profile attributes: H.264 `eAVEncH264VProfile_High`, HEVC `eAVEncH265VProfile_Main_420_8`, AV1 `eAVEncAV1VProfile_Main_420_8` (all in Windows SDK 10.0.26100 `codecapi.h`).
- Windows-only tests (`MediaFoundationGpuVideoEncoderTest`) must be run on a real `COREVIDEO_WITH_MF_ENCODER=ON` dev build before the PR is opened; CI cannot compile them (CLAUDE.md).
- Build the tests with `--config Release` and run `native/build-dev/corevideo-native-tests.exe` from the worktree's own `build-dev` (never another worktree's). The dev worktree for this plan already has a full `build-dev` from `npm run build:native-dev`.
- Never run the native test suite while the owner's installed CoreVideo Pro is streaming or has the virtual camera on unless PR #563 is merged into the branch (its test-isolation fix is what keeps the suite from unlinking the live vcam slot). Branch this plan from `main` AFTER #563 and #564 merge, or cherry-pick 312b631c first.

---

### Task 1: Codec on the encoder seam and the path decision

**Files:**
- Modify: `native/src/modules/GpuVideoEncoder.h` (`GpuVideoEncoderConfig`, `chooseStreamEncodePath`)
- Test: `native/tests/GpuVideoEncoderPolicyTest.cpp`

**Interfaces:**
- Produces: `GpuVideoEncoderConfig::codec` (`std::string`, default `"h264"`); `GpuEncodePath chooseStreamEncodePath(const GpuEncodePathInputs& base, std::string_view codec, bool codecHasGpuEncoder, bool frameHasEncoderTexture, const char** reason)`. Reasons: base reasons unchanged, then `"no-hardware-encoder"` when `!codecHasGpuEncoder`, then `"no-encoder-texture"`, else `"gpu-direct"`. `"codec-not-h264"` no longer exists.

- [ ] **Step 1: Replace the two `ChooseStreamEncodePath` tests that pin the old signature and add the codec cases**

In `native/tests/GpuVideoEncoderPolicyTest.cpp`, delete `GpuDirectWhenEverythingAligns`, `BaseBlockerKeepsPrecedenceOverSenderGates`, `NonH264CodecFallsBackEvenWhenCapable`, `MissingEncoderTextureFallsBack`, `EnvToggleFallsBackWithEnvReason` and replace them with:

```cpp
// chooseStreamEncodePath: the sender's start-time decision. base = {hardware,
// session, forcedOffByEnv, platformSupported}. The codec is the one ACTUALLY
// SENT (resolved through RtmpCompatibility), and codecHasGpuEncoder is what the
// capacity probe says about THAT codec on this machine.
using corevideo::modules::chooseStreamEncodePath;

TEST(ChooseStreamEncodePath, GpuDirectForEveryShippedCodecWhenEverythingAligns) {
  for (const char* codec : {"h264", "hevc", "av1"}) {
    const char* reason = nullptr;
    EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, codec,
                                     /*codecHasGpuEncoder=*/true,
                                     /*frameHasEncoderTexture=*/true, &reason),
              GpuEncodePath::GpuDirect) << codec;
    EXPECT_EQ(std::string(reason), "gpu-direct") << codec;
  }
}

TEST(ChooseStreamEncodePath, BaseBlockerKeepsPrecedenceOverSenderGates) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({/*hw=*/false, true, false, true}, "av1",
                                   /*codecHasGpuEncoder=*/false,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-hardware-encoder");
}

// 2026-09-20: "codec-not-h264" is retired. A codec this machine has no hardware
// encoder for reports the SAME reason as no hardware at all, so support bundles
// keep one vocabulary.
TEST(ChooseStreamEncodePath, ACodecWithoutAGpuEncoderReportsNoHardwareEncoder) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, "hevc",
                                   /*codecHasGpuEncoder=*/false,
                                   /*frameHasEncoderTexture=*/true, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-hardware-encoder");
}

TEST(ChooseStreamEncodePath, MissingEncoderTextureFallsBack) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, false, true}, "h264", true,
                                   /*frameHasEncoderTexture=*/false, &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "no-encoder-texture");
}

TEST(ChooseStreamEncodePath, EnvToggleFallsBackWithEnvReason) {
  const char* reason = nullptr;
  EXPECT_EQ(chooseStreamEncodePath({true, true, /*forcedOffByEnv=*/true, true}, "h264", true, true,
                                   &reason),
            GpuEncodePath::CpuFallback);
  EXPECT_EQ(std::string(reason), "forced-off-by-env");
}

TEST(GpuVideoEncoderConfig, CodecDefaultsToH264) {
  corevideo::modules::GpuVideoEncoderConfig cfg;
  EXPECT_EQ(cfg.codec, "h264");
}
```

- [ ] **Step 2: Build the tests and run them to verify they fail to compile**

Run (PowerShell, from the worktree root):
```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error "
```
Expected: `error C2664` (or similar) at `chooseStreamEncodePath` — no overload takes a `const char*` codec — and `'codec': is not a member of 'GpuVideoEncoderConfig'`.

- [ ] **Step 3: Add the field and change the signature**

In `native/src/modules/GpuVideoEncoder.h`, add `#include <string_view>` next to `<string>`, then in `GpuVideoEncoderConfig` after `h264Profile`:

```cpp
  // 2026-09-20: the codec the hardware MFT is bound for. Normalized: "h264",
  // "hevc" or "av1" ("h265" is the operator-settings spelling; the sender maps
  // it). The Windows implementation enumerates the MFT for this subtype and, for
  // HEVC, disables B-frames — the FLV muxer refuses reordered raw HEVC.
  std::string codec = "h264";
```

Replace `chooseStreamEncodePath` (the whole function and its comment) with:

```cpp
// The stream sender's start-time decision. It folds two sender-specific gates
// into the base policy, applied ONLY when the base policy already allows
// GPU-direct so the most specific base blocker (no hardware, no session, env
// off) keeps precedence in the reason. `codec` is the codec ACTUALLY SENT
// (resolved through RtmpCompatibility) and `codecHasGpuEncoder` is what the
// capacity probe says about that codec on this machine — since 2026-09-20 every
// shipped codec (h264/hevc/av1) can take this path, so a codec with no hardware
// encoder reports the same "no-hardware-encoder" as no hardware at all (the old
// "codec-not-h264" is retired). The compositor must also be exporting the
// dedicated encoder texture on the frame that starts the process (otherwise the
// encoder has nothing to open and the bitstream-mode muxer would stall).
[[nodiscard]] inline GpuEncodePath chooseStreamEncodePath(const GpuEncodePathInputs& base,
                                                          std::string_view codec,
                                                          bool codecHasGpuEncoder,
                                                          bool frameHasEncoderTexture,
                                                          const char** reason) {
  (void)codec;  // carried for logging/diagnostics by callers; the decision is the two bools
  if (GpuEncodePathPolicy::choose(base) == GpuEncodePath::CpuFallback) {
    if (reason) *reason = GpuEncodePathPolicy::reason(base);
    return GpuEncodePath::CpuFallback;
  }
  if (!codecHasGpuEncoder) {
    if (reason) *reason = "no-hardware-encoder";
    return GpuEncodePath::CpuFallback;
  }
  if (!frameHasEncoderTexture) {
    if (reason) *reason = "no-encoder-texture";
    return GpuEncodePath::CpuFallback;
  }
  if (reason) *reason = "gpu-direct";
  return GpuEncodePath::GpuDirect;
}
```

Also update the header's top comment line "emitting a ~6 Mbps H.264 elementary-stream bitstream" to "emitting an H.264, HEVC or AV1 elementary-stream bitstream (codec on the config)".

- [ ] **Step 4: Fix the one production caller so the tree compiles (temporary shim; Task 7 replaces it)**

In `native/src/modules/RtmpOutputSenderAdapter.cpp` `resolveGpuEncodePath` (around line 1561), replace:
```cpp
    const bool codecIsH264 = compatibility.videoCodec == "h264";
    const bool frameHasEncoderTexture = !frame.encoderSharedTexture.sharedHandleHex.empty();
    const char* reason = "cpu-fallback";
    const auto path = chooseStreamEncodePath(in, codecIsH264, frameHasEncoderTexture, &reason);
```
with:
```cpp
    const bool codecIsH264 = compatibility.videoCodec == "h264";  // Task 7 widens this to the probe
    const bool frameHasEncoderTexture = !frame.encoderSharedTexture.sharedHandleHex.empty();
    const char* reason = "cpu-fallback";
    const auto path = chooseStreamEncodePath(in, compatibility.videoCodec == "h265" ? "hevc" : compatibility.videoCodec,
                                             /*codecHasGpuEncoder=*/codecIsH264, frameHasEncoderTexture, &reason);
```
Behavior is unchanged for now (only H.264 takes the GPU path) but the reason for HEVC/AV1 is now `no-hardware-encoder`.

- [ ] **Step 5: Build and run the policy tests**

Run the build command from Step 2, then:
```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='ChooseStreamEncodePath.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='GpuVideoEncoderConfig.*'
native\build-dev\corevideo-native-tests.exe --gtest_filter='GpuEncodePathPolicy.*'
```
Expected: every listed test `[  PASSED  ]`, `0 failed` in each run.

- [ ] **Step 6: Commit**

```bash
git add native/src/modules/GpuVideoEncoder.h native/src/modules/RtmpOutputSenderAdapter.cpp native/tests/GpuVideoEncoderPolicyTest.cpp
git commit -m "GPU encoder seam: codec on the config, path decision per codec (codec-not-h264 retired)"
```

---

### Task 2: RtmpCompatibility refuses instead of downgrading

**Files:**
- Modify: `native/src/modules/RtmpCompatibility.h`
- Test: `native/tests/RtmpCompatibilityTest.cpp`

**Interfaces:**
- Produces: `RtmpCompatibilityResult::refused` (`bool`), `RtmpCompatibilityResult::reason` (`std::string`, `"enhanced-rtmp-required"` when refused, else empty). `videoCodec` always equals `requestedVideoCodec`; `fallbackApplied` is never set by this resolver (field kept).
- Consumed by Task 6 (`StreamStartAdmission`) and Task 7 (sender).

- [ ] **Step 1: Rewrite the three fallback tests as refusal tests**

In `native/tests/RtmpCompatibilityTest.cpp` replace `H265FallsBackToH264ByDefault`, `Av1FallsBackToH264ByDefault` and `HevcAliasNormalizesToH265ThenFallsBack` with:

```cpp
// 2026-09-20: a codec the destination cannot take is REFUSED, never downgraded.
// The silent H.265 -> H.264 downgrade delivered the wrong codec on the slow raw
// path and read to the operator as "YouTube is not getting enough data".
TEST(RtmpCompatibility, H265WithoutEnhancedRtmpIsRefusedNotDowngraded) {
  const auto resolved = resolveRtmpCompatibility("h265", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h265");  // the codec asked for, not a substitute
  EXPECT_TRUE(resolved.refused);
  EXPECT_EQ(resolved.reason, "enhanced-rtmp-required");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_FALSE(resolved.enhancedRtmp);
  EXPECT_NE(resolved.warning.find("Enhanced RTMP"), std::string::npos);
  EXPECT_NE(resolved.warning.find("H.265"), std::string::npos);
}

TEST(RtmpCompatibility, Av1WithoutEnhancedRtmpIsRefusedNotDowngraded) {
  const auto resolved = resolveRtmpCompatibility("av1", false);
  EXPECT_EQ(resolved.videoCodec, "av1");
  EXPECT_TRUE(resolved.refused);
  EXPECT_EQ(resolved.reason, "enhanced-rtmp-required");
  EXPECT_FALSE(resolved.fallbackApplied);
  EXPECT_NE(resolved.warning.find("AV1"), std::string::npos);
}

TEST(RtmpCompatibility, HevcAliasNormalizesToH265ThenIsRefusedWithoutEnhancedRtmp) {
  const auto resolved = resolveRtmpCompatibility("HEVC", false);
  EXPECT_EQ(resolved.requestedVideoCodec, "h265");
  EXPECT_EQ(resolved.videoCodec, "h265");
  EXPECT_TRUE(resolved.refused);
}

TEST(RtmpCompatibility, H264IsNeverRefused) {
  const auto resolved = resolveRtmpCompatibility("h264", false);
  EXPECT_FALSE(resolved.refused);
  EXPECT_TRUE(resolved.reason.empty());
}
```

Also in `H265OverEnhancedRtmpKeepsCodecWithAdvisory` and `Av1OverEnhancedRtmpKeepsCodec` add `EXPECT_FALSE(resolved.refused);`.

- [ ] **Step 2: Build and run to verify the new tests fail**

Build as in Task 1 Step 2, then:
```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpCompatibility.*'
```
Expected: compile error `'refused': is not a member` (fix nothing yet — this is the RED).

- [ ] **Step 3: Implement the refusal**

In `native/src/modules/RtmpCompatibility.h`, add to `RtmpCompatibilityResult` after `warning`:

```cpp
  // 2026-09-20: a request the destination cannot carry is REFUSED at stream
  // start, never downgraded. `reason` is a stable code the sender publishes as
  // lastResultCode ("enhanced-rtmp-required"); `warning` is the operator sentence.
  bool refused = false;
  std::string reason;
```

Replace the `else` branch of `resolveRtmpCompatibility` (the one that set `videoCodec = "h264"` and `fallbackApplied = true`) with:

```cpp
  } else {
    result.refused = true;
    result.reason = "enhanced-rtmp-required";
    result.warning = label + " over RTMP needs Enhanced RTMP; enable it in Stream settings or choose H.264.";
  }
```

Update the file's top comment: replace "or downgrades to H.264 with a clear fallback warning so the stream always plays." with "or REFUSES the start with a named reason (2026-09-20: the silent downgrade delivered the wrong codec on the slow raw path; a stream starts with the codec the operator chose or it does not start)."

- [ ] **Step 4: Build and run**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpCompatibility.*'
```
Expected: 8 tests passed, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/RtmpCompatibility.h native/tests/RtmpCompatibilityTest.cpp
git commit -m "RtmpCompatibility: refuse HEVC/AV1 without enhanced RTMP instead of downgrading to H.264"
```

---

### Task 3: EncoderPolicy ships HEVC on Windows (owner reversal, dated)

**Files:**
- Modify: `native/src/modules/EncoderPolicy.h`
- Test: `native/tests/EncoderPolicyTest.cpp`

**Interfaces:**
- Produces: `codecHasSupportedHardwareEncoder("h265")` → `true` on Windows, `false` on Apple; `isSupportedEncoder("hevc_nvenc")` → `true`; `preferredEncoderFor("h265","nvenc")` → `"hevc_nvenc"`; `encoderCandidatesFor("h265","auto")` → `{"hevc_nvenc"}` on Windows.

- [ ] **Step 1: Invert the HEVC test and add the Windows candidate test**

In `native/tests/EncoderPolicyTest.cpp` replace `HevcEncodeIsNotShippedAnywhere` with:

```cpp
// OWNER REVERSAL 2026-09-20: HEVC encode SHIPS on Windows (NVENC). The
// 2026-08-06 exclusion (patent exposure) was reversed by the owner with that
// exposure accepted; macOS still has no HEVC path in this product.
TEST(EncoderPolicy, HevcShipsOnWindowsNvencOnly) {
#if defined(__APPLE__)
  EXPECT_FALSE(codecHasSupportedHardwareEncoder("h265"));
#else
  EXPECT_TRUE(codecHasSupportedHardwareEncoder("h265"));
  EXPECT_TRUE(isSupportedEncoder("hevc_nvenc"));
  EXPECT_EQ(preferredEncoderFor("h265", "nvenc"), "hevc_nvenc");
  EXPECT_EQ(preferredEncoderFor("h265", "auto"), "hevc_nvenc");
  const auto candidates = encoderCandidatesFor("h265", "auto");
  ASSERT_EQ(candidates.size(), 1u);  // NVENC or nothing: no software HEVC, no h264 substitute
  EXPECT_EQ(candidates[0], "hevc_nvenc");
#endif
}
```

If `NeverOffersGplOrRoyaltyEncumberedSoftwareEncoders` asserts `libx265` is absent, leave it: software HEVC stays out.

- [ ] **Step 2: Build and run to verify it fails**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderPolicy.*'
```
Expected: `HevcShipsOnWindowsNvencOnly` FAILED (`codecHasSupportedHardwareEncoder("h265")` is false).

- [ ] **Step 3: Implement**

In `native/src/modules/EncoderPolicy.h`:

`isSupportedEncoder`:
```cpp
inline bool isSupportedEncoder(const std::string& encoder) {
  return encoder == "h264_nvenc" || encoder == "hevc_nvenc" || encoder == "av1_nvenc" ||
         encoder == "h264_videotoolbox" || encoder == "h264_mf";
}
```

`codecHasSupportedHardwareEncoder`:
```cpp
inline bool codecHasSupportedHardwareEncoder(const std::string& normalizedCodec) {
#if defined(__APPLE__)
  return normalizedCodec == "h264";
#else
  // HEVC ships on Windows since 2026-09-20 (owner reversal, see the header comment).
  return normalizedCodec == "h264" || normalizedCodec == "h265" || normalizedCodec == "av1";
#endif
}
```

`preferredEncoderFor`, the `nvenc` branch and the non-Apple default:
```cpp
  if (normalizedMode == "nvenc") {
    if (normalizedCodec == "av1") return "av1_nvenc";
    if (normalizedCodec == "h265") return "hevc_nvenc";
    return "h264_nvenc";
  }
  ...
#else
  if (normalizedCodec == "av1") return "av1_nvenc";
  if (normalizedCodec == "h265") return "hevc_nvenc";
  return "h264_nvenc";
#endif
```

`encoderCandidatesFor`, the non-Apple branch:
```cpp
  if (normalizedCodec == "av1") {
    return {"av1_nvenc"};
  }
  if (normalizedCodec == "h265") {
    return {"hevc_nvenc"};  // NVENC or nothing: no software HEVC, never an H.264 substitute
  }
  return {"h264_nvenc", "h264_mf"};
```

In the header comment, replace the line `// not tested. HEVC/H.265 ENCODE is not shipped at all.` with:
```cpp
// not tested. HEVC/H.265 ENCODE was not shipped at all from 2026-08-06 to
// 2026-09-20; the OWNER REVERSED that on 2026-09-20 with the patent exposure
// below accepted, so HEVC ships on Windows via NVENC (hevc_nvenc / the NVIDIA
// HEVC Encoder MFT on the GPU-direct path). Not on macOS. The bullet below is
// kept as the record of why it was excluded, not as current policy.
```

- [ ] **Step 4: Build and run**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderPolicy.*'
```
Expected: all `EncoderPolicy.*` pass, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/EncoderPolicy.h native/tests/EncoderPolicyTest.cpp
git commit -m "EncoderPolicy: ship HEVC on Windows NVENC (owner reversal 2026-09-20, patent exposure accepted)"
```

---

### Task 4: FFmpeg bitstream mode names the raw demuxer per codec

**Files:**
- Modify: `native/src/modules/RtmpFfmpegArgs.h` (`RtmpFfmpegArgsConfig`, `buildRtmpFfmpegArguments`)
- Test: `native/tests/RtmpFfmpegArgsTest.cpp`

**Interfaces:**
- Produces: `RtmpFfmpegArgsConfig::videoBitstreamCodec` (`std::string`, default `"h264"`, values `"h264"|"hevc"|"av1"`); `rawDemuxerForBitstreamCodec(std::string_view)` → `"h264"|"hevc"|"obu"` (unknown → `"h264"`).

- [ ] **Step 1: Add the failing tests**

In `native/tests/RtmpFfmpegArgsTest.cpp` after `BitstreamInputModeCopiesVideoAndSkipsRawEncode`:

```cpp
// 2026-09-20: GPU-direct HEVC/AV1. The raw elementary stream on pipe:0 needs the
// matching raw demuxer; -c:v copy into FLV is unchanged and this FFmpeg writes
// the enhanced-RTMP fourcc itself (no -tag:v).
TEST(RtmpFfmpegArgs, BitstreamInputModeNamesTheRawDemuxerPerCodec) {
  using corevideo::modules::rawDemuxerForBitstreamCodec;
  EXPECT_EQ(rawDemuxerForBitstreamCodec("h264"), "h264");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("hevc"), "hevc");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("av1"), "obu");
  EXPECT_EQ(rawDemuxerForBitstreamCodec("bogus"), "h264");

  for (const auto& [codec, demuxer] : {std::pair{"hevc", "hevc"}, std::pair{"av1", "obu"}}) {
    corevideo::modules::RtmpFfmpegArgsConfig config;
    config.videoBitstreamInput = true;
    config.videoBitstreamCodec = codec;
    config.fps = 60;
    config.hasAudio = true;
    config.audioInput = "pipe:3";
    const auto args = corevideo::modules::buildRtmpFfmpegArguments(config);
    EXPECT_NE(args.find(std::string("-use_wallclock_as_timestamps 1 -r 60 -f ") + demuxer +
                        " -thread_queue_size 512 -i pipe:0"),
              std::string::npos) << codec << " :: " << args;
    EXPECT_NE(args.find("-c:v copy"), std::string::npos) << codec;
    EXPECT_EQ(args.find("-tag:v"), std::string::npos) << codec;
    EXPECT_EQ(args.find("-f h264 "), std::string::npos) << codec;
  }
}
```

- [ ] **Step 2: Build and run to verify failure**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpFfmpegArgs.*'
```
Expected: compile error (`videoBitstreamCodec` / `rawDemuxerForBitstreamCodec` undefined).

- [ ] **Step 3: Implement**

In `native/src/modules/RtmpFfmpegArgs.h`, in `RtmpFfmpegArgsConfig` after `videoBitstreamInput`:

```cpp
  // 2026-09-20: which raw elementary stream arrives on pipe:0 in bitstream mode.
  // "h264" (Annex-B), "hevc" (Annex-B, B-frames OFF — the FLV muxer refuses
  // reordered raw HEVC) or "av1" (low-overhead OBU). Selects the raw demuxer only.
  std::string videoBitstreamCodec = "h264";
```

Above `buildRtmpFfmpegArguments` add:

```cpp
// The raw-stream demuxer FFmpeg needs for a GPU-direct bitstream on pipe:0.
// Unknown spellings fall back to h264, the path every build has shipped.
inline const char* rawDemuxerForBitstreamCodec(std::string_view codec) {
  if (codec == "hevc" || codec == "h265") return "hevc";
  if (codec == "av1") return "obu";
  return "h264";
}
```
(add `#include <string_view>` at the top if absent.)

In the bitstream branch replace:
```cpp
         << " -f h264 -thread_queue_size 512 -i pipe:0";
```
with:
```cpp
         << " -f " << rawDemuxerForBitstreamCodec(config.videoBitstreamCodec)
         << " -thread_queue_size 512 -i pipe:0";
```
and update the branch comment's first line to "GPU-direct path: video arrives already encoded (H.264/HEVC Annex-B or AV1 OBU) on pipe:0".

- [ ] **Step 4: Build and run**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpFfmpegArgs.*'
```
Expected: all pass, including the unchanged `BitstreamInputModeCopiesVideoAndSkipsRawEncode` (default codec is still h264).

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/RtmpFfmpegArgs.h native/tests/RtmpFfmpegArgsTest.cpp
git commit -m "RtmpFfmpegArgs: raw demuxer per bitstream codec (h264/hevc/obu)"
```

---

### Task 5: Capacity probe knows AV1 and a canonical codec name

**Files:**
- Modify: `native/src/modules/EncoderCapacityProbe.h`, `native/src/modules/EncoderCapacityProbe.cpp`
- Test: `native/tests/EncoderCapacityProbePolicyTest.cpp` (create) — register it in `native/CMakeLists.txt` next to `tests/GpuVideoEncoderPolicyTest.cpp` (line ~613)

**Interfaces:**
- Produces: `std::string canonicalProbeCodec(std::string_view)` in `EncoderCapacityProbe.h`: `"h264"`→`"h264"`, `"h265"|"hevc"|"hvc1"`→`"hevc"`, `"av1"|"av01"`→`"av1"`, anything else→`"h264"`. The Windows probe maps `"av1"` to `MFVideoFormat_AV1`.
- Consumed by Task 7 (probe key per codec).

- [ ] **Step 1: Write the failing test**

Create `native/tests/EncoderCapacityProbePolicyTest.cpp`:

```cpp
#include "modules/EncoderCapacityProbe.h"

#include <gtest/gtest.h>

// The probe cache is keyed by codec. One canonical spelling per codec, or the
// same workload is probed twice under two names and the sender's "no free
// session" answer depends on which spelling asked.
TEST(EncoderCapacityProbePolicy, CanonicalProbeCodecCollapsesSpellings) {
  using corevideo::modules::canonicalProbeCodec;
  EXPECT_EQ(canonicalProbeCodec("h264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("H264"), "h264");
  EXPECT_EQ(canonicalProbeCodec("h265"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hevc"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("hvc1"), "hevc");
  EXPECT_EQ(canonicalProbeCodec("av1"), "av1");
  EXPECT_EQ(canonicalProbeCodec("av01"), "av1");
  EXPECT_EQ(canonicalProbeCodec(""), "h264");
  EXPECT_EQ(canonicalProbeCodec("bogus"), "h264");
}

TEST(EncoderCapacityProbePolicy, ProbeKeyOrdersByCodecFirst) {
  corevideo::modules::EncoderProbeKey a{"av1", 1920, 1080, 60};
  corevideo::modules::EncoderProbeKey b{"h264", 1920, 1080, 60};
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}
```

Add `    tests/EncoderCapacityProbePolicyTest.cpp` to the test source list in `native/CMakeLists.txt` directly after `tests/GpuVideoEncoderPolicyTest.cpp` (it must be in the unconditional list, not behind `COREVIDEO_WITH_MF_ENCODER`).

- [ ] **Step 2: Build and run to verify failure**

Re-run CMake configure so the new file is picked up, then build:
```powershell
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake -S native -B native\build-dev >nul && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error "
```
Expected: `'canonicalProbeCodec': is not a member`.

- [ ] **Step 3: Implement**

In `native/src/modules/EncoderCapacityProbe.h`, add `#include <string_view>` and `#include <cctype>`, and below `EncoderProbeKey`:

```cpp
// One canonical spelling per codec for the cache key. "h264" / "hevc" / "av1";
// anything unknown is "h264", the workload every build has always probed.
inline std::string canonicalProbeCodec(std::string_view codec) {
  std::string lowered(codec);
  for (auto& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (lowered == "h265" || lowered == "hevc" || lowered == "hvc1") return "hevc";
  if (lowered == "av1" || lowered == "av01") return "av1";
  return "h264";
}
```
Change the `EncoderProbeKey::codec` comment to `// canonical name ("h264" / "hevc" / "av1"), see canonicalProbeCodec`.

In `native/src/modules/EncoderCapacityProbe.cpp` `subtypeForCodec`:
```cpp
GUID subtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265" || codec == "hvc1") return MFVideoFormat_HEVC;
  if (codec == "av1" || codec == "av01") return MFVideoFormat_AV1;
  return MFVideoFormat_H264;
}
```
Also find the `MF_MT_MPEG2_PROFILE` (or equivalent output-type profile) line in the probe's session-creation code and make it per codec: H.264 `eAVEncH264VProfile_High`, HEVC `eAVEncH265VProfile_Main_420_8`, AV1 `eAVEncAV1VProfile_Main_420_8`. If the probe sets no profile, leave it.

- [ ] **Step 4: Build and run**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='EncoderCapacityProbePolicy.*'
```
Expected: 2 passed, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/EncoderCapacityProbe.h native/src/modules/EncoderCapacityProbe.cpp native/tests/EncoderCapacityProbePolicyTest.cpp native/CMakeLists.txt
git commit -m "EncoderCapacityProbe: AV1 workload + one canonical codec spelling for the cache key"
```

---

### Task 6: StreamStartAdmission — the pure refuse-never-downgrade decision

**Files:**
- Create: `native/src/modules/StreamStartAdmission.h`
- Test: `native/tests/StreamStartAdmissionTest.cpp` (create; register in `native/CMakeLists.txt` after `tests/EncoderCapacityProbePolicyTest.cpp`)

**Interfaces:**
- Produces:
```cpp
namespace corevideo::modules {
struct StreamStartAdmissionInputs {
  std::string requestedCodec;      // RtmpCompatibility spelling: "h264" | "h265" | "av1"
  bool compatibilityRefused = false;  // RtmpCompatibilityResult::refused
  std::string compatibilityReason;    // RtmpCompatibilityResult::reason
  bool codecHasHardwareEncoder = true;  // codecHasSupportedHardwareEncoder + probe (not pending)
  bool gpuPathChosen = true;          // chooseStreamEncodePath == GpuDirect
  const char* gpuPathReason = "gpu-direct";
  bool gpuEncoderStartFailed = false; // GpuVideoEncoder::start() returned false
  std::string gpuEncoderFailureDetail;
};
struct StreamStartAdmission {
  bool refused = false;
  std::string resultCode;   // "enhanced-rtmp-required" | "no-hardware-encoder" | "gpu-encoder-start-failed" | ""
  std::string message;      // operator sentence; empty when admitted
};
[[nodiscard]] StreamStartAdmission admitStreamStart(const StreamStartAdmissionInputs& in);
}
```
- Rules (spec §5): refuse in this order — compatibility refusal; then no hardware encoder for the requested codec; then GPU encoder start failed (any codec). A CPU-fallback path for H.264 with reason `platform-unsupported` / `forced-off-by-env` / `no-encoder-texture` is ADMITTED (today's behavior). A CPU-fallback path for `h265`/`av1` for any reason is refused as `no-hardware-encoder`.
- Consumed by Task 7.

- [ ] **Step 1: Write the failing tests**

Create `native/tests/StreamStartAdmissionTest.cpp`:

```cpp
#include "modules/StreamStartAdmission.h"

#include <gtest/gtest.h>

using corevideo::modules::admitStreamStart;
using corevideo::modules::StreamStartAdmissionInputs;

namespace {
StreamStartAdmissionInputs healthy(const char* codec) {
  StreamStartAdmissionInputs in;
  in.requestedCodec = codec;
  return in;
}
}  // namespace

TEST(StreamStartAdmission, EveryShippedCodecIsAdmittedWhenEverythingAligns) {
  for (const char* codec : {"h264", "h265", "av1"}) {
    const auto a = admitStreamStart(healthy(codec));
    EXPECT_FALSE(a.refused) << codec;
    EXPECT_TRUE(a.resultCode.empty()) << codec;
    EXPECT_TRUE(a.message.empty()) << codec;
  }
}

TEST(StreamStartAdmission, ACompatibilityRefusalWinsAndNamesEnhancedRtmp) {
  auto in = healthy("h265");
  in.compatibilityRefused = true;
  in.compatibilityReason = "enhanced-rtmp-required";
  in.codecHasHardwareEncoder = false;  // would also refuse; compatibility is checked first
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "enhanced-rtmp-required");
  EXPECT_NE(a.message.find("Enhanced RTMP"), std::string::npos);
  EXPECT_NE(a.message.find("H.265"), std::string::npos);
}

TEST(StreamStartAdmission, NoHardwareEncoderForTheCodecRefusesAndNamesTheCodec) {
  auto in = healthy("av1");
  in.codecHasHardwareEncoder = false;
  in.gpuPathChosen = false;
  in.gpuPathReason = "no-hardware-encoder";
  const auto a = admitStreamStart(in);
  EXPECT_TRUE(a.refused);
  EXPECT_EQ(a.resultCode, "no-hardware-encoder");
  EXPECT_NE(a.message.find("AV1"), std::string::npos);
  EXPECT_NE(a.message.find("hardware encoder"), std::string::npos);
}

// The raw path cannot hold 1080p60 today (spec: fallback throughput is
// sub-project 2), so HEVC/AV1 are GPU-direct or nothing — for ANY fallback reason.
TEST(StreamStartAdmission, HevcOrAv1OnTheCpuFallbackIsRefusedForAnyReason) {
  for (const char* reason : {"forced-off-by-env", "no-encoder-texture", "platform-unsupported",
                             "no-free-encoder-session"}) {
    auto in = healthy("h265");
    in.gpuPathChosen = false;
    in.gpuPathReason = reason;
    const auto a = admitStreamStart(in);
    EXPECT_TRUE(a.refused) << reason;
    EXPECT_EQ(a.resultCode, "no-hardware-encoder") << reason;
    EXPECT_NE(a.message.find(reason), std::string::npos) << reason;  // the path reason is quoted
  }
}

// H.264 keeps every path it has today: a machine on the raw fallback for a
// platform/env/texture reason still streams H.264 exactly as before.
TEST(StreamStartAdmission, H264OnTheCpuFallbackIsStillAdmitted) {
  for (const char* reason : {"forced-off-by-env", "no-encoder-texture", "platform-unsupported",
                             "no-free-encoder-session", "no-hardware-encoder"}) {
    auto in = healthy("h264");
    in.gpuPathChosen = false;
    in.gpuPathReason = reason;
    const auto a = admitStreamStart(in);
    EXPECT_FALSE(a.refused) << reason;
  }
}

TEST(StreamStartAdmission, AGpuEncoderStartFailureRefusesForEveryCodecAndQuotesTheDetail) {
  for (const char* codec : {"h264", "h265", "av1"}) {
    auto in = healthy(codec);
    in.gpuEncoderStartFailed = true;
    in.gpuEncoderFailureDetail = "set-output-type hr=0xC00D36B4";
    const auto a = admitStreamStart(in);
    EXPECT_TRUE(a.refused) << codec;
    EXPECT_EQ(a.resultCode, "gpu-encoder-start-failed") << codec;
    EXPECT_NE(a.message.find("0xC00D36B4"), std::string::npos) << codec;
  }
}
```

Register `    tests/StreamStartAdmissionTest.cpp` in `native/CMakeLists.txt` in the unconditional test list.

- [ ] **Step 2: Configure, build, verify failure**

Same configure+build command as Task 5 Step 2. Expected: `Cannot open include file: 'modules/StreamStartAdmission.h'`.

- [ ] **Step 3: Implement**

Create `native/src/modules/StreamStartAdmission.h`:

```cpp
#pragma once

// STREAM START ADMISSION (2026-09-20): refuse, never downgrade.
//
// On 2026-09-20 an H.265 request was silently turned into H.264 on the raw-pipe
// fallback, which delivers ~0.87x of real time at 1080p60; YouTube reported it
// was not receiving enough data and the operator had no idea why. Rule: a
// stream starts with the codec the operator chose, on a path that can carry it,
// or it does not start — and the refusal names why, in a stable code the shell
// renders. Pure and header-only (the CaptureReaderStallPolicy shape) so the
// decision is unit-tested without FFmpeg, a GPU or a destination.
//
// Order of checks (the most operator-actionable first):
//   1. compatibility refusal   -> "enhanced-rtmp-required" (turn the checkbox on)
//   2. no hardware encoder     -> "no-hardware-encoder"    (this machine cannot)
//      (HEVC/AV1 on ANY CPU-fallback reason land here too: the raw path cannot
//       hold 1080p60 until sub-project 2, and it has no HEVC/AV1 encoder we ship)
//   3. GPU encoder start failed-> "gpu-encoder-start-failed" (HRESULT quoted)
// H.264 on the CPU fallback is ADMITTED: every machine that streams today keeps
// streaming exactly as it does.

#include <string>

namespace corevideo::modules {

struct StreamStartAdmissionInputs {
  std::string requestedCodec;          // RtmpCompatibility spelling: "h264" | "h265" | "av1"
  bool compatibilityRefused = false;   // RtmpCompatibilityResult::refused
  std::string compatibilityReason;     // RtmpCompatibilityResult::reason
  bool codecHasHardwareEncoder = true; // codecHasSupportedHardwareEncoder AND the probe (when not pending)
  bool gpuPathChosen = true;           // chooseStreamEncodePath(...) == GpuDirect
  const char* gpuPathReason = "gpu-direct";
  bool gpuEncoderStartFailed = false;  // GpuVideoEncoder::start() returned false
  std::string gpuEncoderFailureDetail;
};

struct StreamStartAdmission {
  bool refused = false;
  std::string resultCode;  // one of the three codes above, or empty when admitted
  std::string message;     // operator sentence, empty when admitted
};

inline const char* operatorCodecLabel(const std::string& codec) {
  if (codec == "h265" || codec == "hevc") return "H.265";
  if (codec == "av1") return "AV1";
  return "H.264";
}

[[nodiscard]] inline StreamStartAdmission admitStreamStart(const StreamStartAdmissionInputs& in) {
  StreamStartAdmission out;
  const std::string label = operatorCodecLabel(in.requestedCodec);
  const bool isH264 = in.requestedCodec == "h264";
  if (in.compatibilityRefused) {
    out.refused = true;
    out.resultCode = in.compatibilityReason.empty() ? "enhanced-rtmp-required" : in.compatibilityReason;
    out.message = label + " over RTMP needs Enhanced RTMP; enable it in Stream settings or choose H.264.";
    return out;
  }
  if (!in.codecHasHardwareEncoder || (!isH264 && !in.gpuPathChosen)) {
    out.refused = true;
    out.resultCode = "no-hardware-encoder";
    out.message = label + " needs a hardware encoder on the GPU-direct path and this machine cannot provide one (" +
                  std::string(in.gpuPathReason ? in.gpuPathReason : "unknown") +
                  "). Choose H.264 or stream from a machine with an NVIDIA " +
                  (in.requestedCodec == "av1" ? "RTX 40-series or newer" : "GPU") + ".";
    return out;
  }
  if (in.gpuEncoderStartFailed) {
    out.refused = true;
    out.resultCode = "gpu-encoder-start-failed";
    out.message = "The " + label + " hardware encoder failed to start (" + in.gpuEncoderFailureDetail +
                  "). The stream was not started.";
    return out;
  }
  return out;
}

}  // namespace corevideo::modules
```

- [ ] **Step 4: Build and run**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='StreamStartAdmission.*'
```
Expected: 6 passed, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/StreamStartAdmission.h native/tests/StreamStartAdmissionTest.cpp native/CMakeLists.txt
git commit -m "StreamStartAdmission: pure refuse-never-downgrade decision with three named codes"
```

---

### Task 7: The MF GPU encoder binds the MFT per codec (HEVC B-frames off)

**Files:**
- Modify: `native/src/modules/MediaFoundationGpuVideoEncoder.cpp` (`subtypeForCodec`, `createEncoder`, `start` log line)
- Test: `native/tests/MediaFoundationGpuVideoEncoderTest.cpp` (Windows-only, real GPU, self-skipping)

**Interfaces:**
- Consumes: `GpuVideoEncoderConfig::codec` (Task 1).
- Produces: the encoder honors `codec`; `start()` fails with `fail("set-bframes-off")` when the HEVC MFT rejects `CODECAPI_AVEncMPVDefaultBPictureCount`; `fail()` detail strings are what Task 8 quotes in `gpuEncoderFailureDetail`. Log line: `[gpu-encode] started %dx%d@%d %dkbps mft=hardware-%s` with the codec.

- [ ] **Step 1: Parametrize the round-trip test per codec (RED for hevc/av1)**

In `native/tests/MediaFoundationGpuVideoEncoderTest.cpp`, rename the body of `DirectSharedTextureH264RoundTrip` into a static helper `runRoundTrip(const char* codec, const char* rawDemuxer, bool alsoMuxToFlv)` (everything from the compositor setup to the luma assertion), with these three changes inside it:

1. The config line becomes:
```cpp
  corevideo::modules::GpuVideoEncoderConfig encoderConfig{
      plan.width, plan.height, plan.fps, 6000, 2.0, "cbr", "high"};
  encoderConfig.codec = codec;
```
2. The decode command uses the demuxer parameter:
```cpp
  const std::string inner = "\"" + ffmpegExe.string() + "\" -v error -f " + rawDemuxer + " -i \"" +
                            rawPath.string() + "\" -f rawvideo -pix_fmt yuv420p \"" +
                            yuvPath.string() + "\"";
```
3. After the luma assertion, when `alsoMuxToFlv`:
```cpp
  if (alsoMuxToFlv) {
    // The FLV muxer refuses reordered raw HEVC ("Packet is missing PTS"). This
    // copy-mux is the on-rig proof that the MFT honoured B-frames OFF.
    const auto flvPath = rawPath.parent_path() / (std::string("gpu-encode-") + codec + ".flv");
    const std::string muxInner = "\"" + ffmpegExe.string() + "\" -v error -y -use_wallclock_as_timestamps 1 -r " +
                                 std::to_string(plan.fps) + " -f " + rawDemuxer + " -i \"" + rawPath.string() +
                                 "\" -c:v copy -f flv \"" + flvPath.string() + "\"";
    const int muxStatus = normalizedSystemExitCode(std::system(("\"" + muxInner + "\"").c_str()));
    EXPECT_EQ(muxStatus, 0) << codec << " raw bitstream did not copy-mux into FLV: " << rawPath.string();
    std::error_code fec;
    std::filesystem::remove(flvPath, fec);
  }
```
Keep the existing "skipping: encoder start unavailable on this machine" early return; it is what makes a rig without the HEVC/AV1 MFT skip loudly.

Then the three tests:
```cpp
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureH264RoundTrip) { runRoundTrip("h264", "h264", false); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureHevcRoundTripMuxesWithoutBFrames) { runRoundTrip("hevc", "hevc", true); }
TEST(MediaFoundationGpuVideoEncoder, DirectSharedTextureAv1RoundTrip) { runRoundTrip("av1", "obu", true); }
```

- [ ] **Step 2: Build and run to verify RED**

Build (`corevideo-native-tests` target), then:
```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='MediaFoundationGpuVideoEncoder.*'
```
Expected: `DirectSharedTextureH264RoundTrip` passes; the HEVC and AV1 tests FAIL at the ffmpeg decode step (the encoder ignores `codec` and emits H.264, which `-f hevc` / `-f obu` cannot decode) — or the HEVC FLV mux fails. Either failure is the RED; note which.

- [ ] **Step 3: Implement per-codec binding**

In `native/src/modules/MediaFoundationGpuVideoEncoder.cpp`:

`subtypeForCodec`:
```cpp
GUID subtypeForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265") return MFVideoFormat_HEVC;
  if (codec == "av1") return MFVideoFormat_AV1;
  return MFVideoFormat_H264;
}

UINT32 profileForCodec(const std::string& codec) {
  if (codec == "hevc" || codec == "h265") return eAVEncH265VProfile_Main_420_8;
  if (codec == "av1") return eAVEncAV1VProfile_Main_420_8;
  return eAVEncH264VProfile_High;
}
```
Add `#include <codecapi.h>` if not already included (it provides the profile enums and `CODECAPI_AVEncMPVDefaultBPictureCount`); link needs no change (`strmiids`/`mfuuid` already linked for the H.264 enums).

In `createEncoder()`:
- `MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtypeForCodec(config_.codec)};`
- the no-MFT failure string becomes `fail(("no-hardware-mft-" + config_.codec).c_str())` (keep `fail`'s signature; if it takes `const char*`, build the string into a local first).
- after `MF_TRANSFORM_ASYNC_UNLOCK` and BEFORE the output type, add:
```cpp
    // HEVC: B-frames OFF. The FLV muxer cannot take a raw HEVC stream with
    // reordered frames ("Packet is missing PTS", measured 2026-09-20), and the
    // GPU-direct path hands FFmpeg a raw Annex-B stream on a pipe. A MFT that
    // refuses the property fails start() — never a stream the muxer rejects
    // twenty frames in. H.264 keeps its shipped behaviour; AV1 muxed cleanly
    // with the MFT default in the 2026-09-20 probe and is proven per rig by the
    // round-trip test.
    if (config_.codec == "hevc" || config_.codec == "h265") {
      ComPtr<ICodecAPI> codecApi;
      if (FAILED(encoder_.As(&codecApi)) || !codecApi) return fail("no-codec-api");
      VARIANT bframes;
      VariantInit(&bframes);
      bframes.vt = VT_UI4;
      bframes.ulVal = 0;
      const HRESULT bhr = codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &bframes);
      if (FAILED(bhr)) return fail("set-bframes-off");
    }
```
- `outType->SetGUID(MF_MT_SUBTYPE, subtypeForCodec(config_.codec));`
- `outType->SetUINT32(MF_MT_MPEG2_PROFILE, profileForCodec(config_.codec));`

In `start()` change the log line to:
```cpp
    ::corevideo::core::nativeLogf("[gpu-encode] started %dx%d@%d %dkbps mft=hardware-%s\n",
                                 config_.width, config_.height, config_.fps, config_.bitrateKbps,
                                 config_.codec.c_str());
```

- [ ] **Step 4: Build and run the real-GPU tests**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='MediaFoundationGpuVideoEncoder.*'
```
Expected on the owner's rig (RTX 4090, all three MFTs registered): 4 passed (3 round-trips + `SubmitFailsWhenNotRunningSoTheSupervisorRestarts`), 0 failed, and the stderr shows `[gpu-encode] started 1280x720@60 6000kbps mft=hardware-hevc` etc. If a codec's MFT is absent the test prints the skip line and returns — record that in the commit message rather than treating it as green.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/MediaFoundationGpuVideoEncoder.cpp native/tests/MediaFoundationGpuVideoEncoderTest.cpp
git commit -m "MF GPU encoder: bind the hardware MFT per codec; HEVC with B-frames off (FLV muxer contract)"
```

---

### Task 8: The sender carries the codec through and refuses at start

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — `gpuEncoderProbeAllows`, `resolveGpuEncodePath`, `startGpuEncoderIfChosen`, `startFfmpegProcess` (config build), the settings-apply block that composes `runtimeDetail_` (lines ~729-755), and the start path around line 1140.

**Interfaces:**
- Consumes: Task 1 (`chooseStreamEncodePath`, `cfg.codec`), Task 2 (`refused`/`reason`), Task 3 (`codecHasSupportedHardwareEncoder`), Task 4 (`videoBitstreamCodec`), Task 5 (`canonicalProbeCodec`), Task 6 (`admitStreamStart`), Task 7 (`fail` details).
- Produces: `sender_.lastResultCode` ∈ {`enhanced-rtmp-required`, `no-hardware-encoder`, `gpu-encoder-start-failed`} on refusal, `sender_.lastError` = the admission message, `appendSendProof(frame, <code>)`; log line `[gpu-encode] path=gpu-direct codec=<codec> ...`; the send-proof `videoCodec`/`ffmpegVideoEncoder` fields agree with what is sent.

There is no unit test seam for the sender's process start (it spawns FFmpeg), so this task's tests are the pure policies already green in Tasks 1-6 plus the gate in Task 10. Keep each edit minimal and re-run the full native suite at the end of the task.

- [ ] **Step 1: Probe per codec**

Replace `gpuEncoderProbeAllows`:
```cpp
  // A hardware encoder session for THIS codec is (probably) available. Never
  // REFUSE on a pending/unknown probe (the TESTER RULE) — encoder->start() is the
  // real gate. `codec` is the RtmpCompatibility spelling; the probe key is canonical.
  bool gpuEncoderProbeAllows(const std::string& codec, int width, int height) const {
    const auto cap = EncoderCapacityCache::instance().lookup(
        EncoderProbeKey{canonicalProbeCodec(codec), width, height, (std::max)(1, configuredFps_)});
    if (!cap.probed) return true;
    return cap.hardwareAvailable && cap.hardwareSessionCeiling > 0;
  }
```
Add `#include "modules/EncoderCapacityProbe.h"` if the file does not already include it (it uses `EncoderCapacityCache`, so it does) and `#include "modules/StreamStartAdmission.h"`.

- [ ] **Step 2: Path decision on the codec actually sent**

Replace the body of `resolveGpuEncodePath` from `const bool probeAllows` down:
```cpp
    const auto compatibility = resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
    const std::string sentCodec = canonicalProbeCodec(compatibility.videoCodec);  // "h264"|"hevc"|"av1"
    const bool probeAllows = gpuEncoderProbeAllows(compatibility.videoCodec, width, height);
    in.hardwareEncoderAvailable = in.platformSupported && probeAllows;
    in.sessionAvailable = probeAllows;
    const bool codecHasGpuEncoder =
        codecHasSupportedHardwareEncoder(normalizeVideoCodec(compatibility.videoCodec)) && probeAllows;
    const bool frameHasEncoderTexture = !frame.encoderSharedTexture.sharedHandleHex.empty();
    const char* reason = "cpu-fallback";
    const auto path = chooseStreamEncodePath(in, sentCodec, codecHasGpuEncoder, frameHasEncoderTexture, &reason);
    gpuEncodePathReason_ = reason;
    gpuEncodeSentCodec_ = sentCodec;
    return path;
```
Add the member `std::string gpuEncodeSentCodec_ = "h264";` next to `gpuEncodePathReason_`.

- [ ] **Step 3: The encoder config and the FFmpeg args carry the codec**

In `startGpuEncoderIfChosen`, after `cfg.h264Profile = ...` add `cfg.codec = gpuEncodeSentCodec_;`, change the failure branch to record the detail:
```cpp
    if (!ok) {
      gpuEncoderFailureDetail_ = gpuEncoder_->lastFailure();  // Task 8 Step 4 declares lastFailure()
      gpuEncoder_.reset();
      useGpuDirect_ = false;
      gpuEncodePathReason_ = "encoder-start-failed";
      gpuEncoderStartFailed_ = true;
      return false;
    }
```
(add members `bool gpuEncoderStartFailed_ = false;` and `std::string gpuEncoderFailureDetail_;`, both reset at the top of `startGpuEncoderIfChosen`), and change the success log line to:
```cpp
    ::corevideo::core::nativeLogf("[gpu-encode] path=gpu-direct codec=%s %dx%d@%d bitrate=%.1fMbps\n",
                                 cfg.codec.c_str(), width, height, cfg.fps, sender_.bitrateMbps);
```
In `startFfmpegProcess` where `config.videoBitstreamInput = useGpuDirect_;` is set, add the next line `config.videoBitstreamCodec = gpuEncodeSentCodec_;`.

- [ ] **Step 4: Refuse at start**

In `startFfmpegProcess`, right after `startGpuEncoderIfChosen(width, height);` and the existing `if (!useGpuDirect_) { nativeLogf(... cpu-fallback ...) }`, insert:
```cpp
    {
      const auto compat = resolveRtmpCompatibility(configuredVideoCodec_, configuredAllowEnhancedRtmp_);
      StreamStartAdmissionInputs admission;
      admission.requestedCodec = compat.requestedVideoCodec;
      admission.compatibilityRefused = compat.refused;
      admission.compatibilityReason = compat.reason;
      admission.codecHasHardwareEncoder = codecHasSupportedHardwareEncoder(compat.requestedVideoCodec);
      admission.gpuPathChosen = useGpuDirect_;
      admission.gpuPathReason = gpuEncodePathReason_.c_str();
      admission.gpuEncoderStartFailed = gpuEncoderStartFailed_;
      admission.gpuEncoderFailureDetail = gpuEncoderFailureDetail_;
      const auto verdict = admitStreamStart(admission);
      if (verdict.refused) {
        stopGpuEncoder();
        sender_.status = "warning";
        sender_.warning = verdict.message;
        sender_.destinationHealth = "warning";
        sender_.lastResultCode = verdict.resultCode;
        sender_.lastError = verdict.message;
        ::corevideo::core::nativeLogf("[gpu-encode] stream start REFUSED code=%s codec=%s reason=%s :: %s\n",
                                     verdict.resultCode.c_str(), compat.requestedVideoCodec.c_str(),
                                     gpuEncodePathReason_.c_str(), verdict.message.c_str());
        return false;
      }
    }
```
`gpuEncoderFailureDetail_` (`std::string` member) is filled in `startGpuEncoderIfChosen` from the encoder's last failure: the `MediaFoundationGpuVideoEncoderImpl::fail` helper already records a detail string internally — expose it as `virtual std::string lastFailure() const { return {}; }` on `GpuVideoEncoder` (default empty, override in the MF impl returning the last `fail()` argument) and read it here. Add that virtual in `GpuVideoEncoder.h` in this step (one line, default-implemented, so no other implementation breaks).

Also delete the `unsupportedCodecWarning_` block in the settings-apply code (lines ~737-755): its "encoding H.264 instead" sentences are now false. Keep `codecCompatibility.warning` appended to `runtimeDetail_` — for an admitted enhanced-RTMP stream it is the advisory "the ingest must support it", which is still true.

- [ ] **Step 5: Send proof and status agree with what is sent**

At the two `"ffmpegVideoEncoder"` send-proof sites (lines ~2080 and ~2097) the fallback expression `ffmpegVideoEncoderFor(resolveRtmpCompatibility(...).videoCodec, ...)` now resolves to `hevc_nvenc`/`av1_nvenc` for those codecs on the raw path; on the GPU path set `selectedFfmpegVideoEncoder_ = "gpu-direct-" + gpuEncodeSentCodec_` right after a successful `startGpuEncoderIfChosen` so the proof line reads `"ffmpegVideoEncoder":"gpu-direct-hevc"` — a reader can no longer see `h264_nvenc` on an HEVC stream.

- [ ] **Step 6: Build everything and run the full native suite**

```powershell
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native 2>&1" | Select-String " error "
native\build-dev\corevideo-native-tests.exe 2>&1 | Select-String '^\[  FAILED|tests passed'
```
Expected: `0 errors`; `N tests passed, 0 failed` where N ≥ 1082 + the new tests from Tasks 1-7. Confirm `native/build-dev/corevideo-native.exe` is Release-sized (~2.3 MB, not ~8 MB).

- [ ] **Step 7: Commit**

```bash
git add native/src/modules/RtmpOutputSenderAdapter.cpp native/src/modules/GpuVideoEncoder.h
git commit -m "Stream sender: codec rides GPU-direct end to end; refuse at start with a named code instead of downgrading"
```

---

### Task 9: The shell names the refusal

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportStatusFormatter.cs` (`FormatStreamingFailureStatus` ladder)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/StudioViewModelAudioStatusTests.cs`

**Interfaces:**
- Consumes: the sender's `lastError` sentences from Task 6 (they contain "Enhanced RTMP", "hardware encoder", "hardware encoder failed to start").
- Produces: three operator sentences that lead with the codec, never with the stream key.

- [ ] **Step 1: Write the failing tests**

In `StudioViewModelAudioStatusTests.cs` next to `FormatStreamingFailureStatus_ADestinationRefusalDoesNotLeadWithTheStreamKey` add:

```csharp
    [Fact]
    public void FormatStreamingFailureStatus_AnEnhancedRtmpRefusalPointsAtTheCheckboxNotTheKey()
    {
        var status = TransportStatusFormatter.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("RTMP output failed. H.265 over RTMP needs Enhanced RTMP; enable it in Stream settings or choose H.264."));
        Assert.Contains("Enhanced RTMP", status, StringComparison.Ordinal);
        Assert.Contains("H.265", status, StringComparison.Ordinal);
        Assert.DoesNotContain("stream key", status, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("server URL", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_ANoHardwareEncoderRefusalNamesTheCodec()
    {
        var status = TransportStatusFormatter.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("RTMP output failed. AV1 needs a hardware encoder on the GPU-direct path and this machine cannot provide one (no-hardware-encoder). Choose H.264 or stream from a machine with an NVIDIA RTX 40-series or newer."));
        Assert.Contains("AV1", status, StringComparison.Ordinal);
        Assert.Contains("hardware encoder", status, StringComparison.Ordinal);
        Assert.DoesNotContain("stream key", status, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void FormatStreamingFailureStatus_AGpuEncoderStartFailureQuotesTheDetail()
    {
        var status = TransportStatusFormatter.FormatStreamingFailureStatus(
            "start",
            new InvalidOperationException("RTMP output failed. The H.265 hardware encoder failed to start (set-bframes-off). The stream was not started."));
        Assert.Contains("hardware encoder failed to start", status, StringComparison.Ordinal);
        Assert.Contains("set-bframes-off", status, StringComparison.Ordinal);
        Assert.DoesNotContain("stream key", status, StringComparison.OrdinalIgnoreCase);
    }
```

- [ ] **Step 2: Run to verify failure**

```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true --filter "FullyQualifiedName~FormatStreamingFailureStatus_A"
```
Expected: the three new tests fail (the generic "Check the server URL, stream key, and network." sentence wins today).

- [ ] **Step 3: Add the branch to the ladder**

In `FormatStreamingFailureStatus`, insert a new ternary arm BEFORE the "destination refused" arm (so a codec refusal is never mistaken for a connection problem):

```csharp
            // A CODEC refusal (2026-09-20, StreamStartAdmission in the core): the
            // core's own sentence already names the codec and the fix; pass it
            // through untouched and never fall into the generic key/URL advice.
            : lowered.Contains("needs enhanced rtmp", StringComparison.Ordinal) ||
              lowered.Contains("needs a hardware encoder", StringComparison.Ordinal) ||
              lowered.Contains("hardware encoder failed to start", StringComparison.Ordinal)
            ? StripRtmpOutputFailedPrefix(detail)
```
and add the helper next to `NormalizeStreamingFailureDetail`:
```csharp
    private static string StripRtmpOutputFailedPrefix(string detail)
    {
        const string prefix = "RTMP output failed.";
        var trimmed = detail.Trim();
        return trimmed.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
            ? trimmed[prefix.Length..].Trim()
            : trimmed;
    }
```
(If the ladder builds `detail` with a different prefix, match that prefix instead — read `NormalizeStreamingFailureDetail` first.) Also add the compact readout for the transport chip, if the ladder has a compact-form table: `"Codec refused"` keyed on the same three substrings.

- [ ] **Step 4: Run the test project**

Same command as Step 2 without `--filter`. Expected: all pass (previously 1510/0 plus 3).

- [ ] **Step 5: Commit**

```bash
git add native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportStatusFormatter.cs native-shell/CoreVideoPro.WinUI.Tests/StudioViewModelAudioStatusTests.cs
git commit -m "Shell: a codec refusal names the codec and the fix, never the stream key"
```

---

### Task 10: The gate script runs per codec; docs

**Files:**
- Modify: `scripts/validate-gpu-encode.mjs` (`--codec`), `CLAUDE.md` (GPU-direct section), `docs/BACKLOG.md` (#521 row)

**Interfaces:**
- Consumes: the wire fields `videoCodec` and `allowEnhancedRtmp` on `start-program-output` destination settings (`MediaCore.cpp:132`, `MediaCoreCommandBuilder.cs:686`).
- Produces: `node scripts/validate-gpu-encode.mjs --codec h264|hevc|av1`.

- [ ] **Step 1: Add `--codec` to the script**

Near `const forceRaw = args.includes("--force-raw");` add:
```js
const codecArgIndex = args.indexOf("--codec");
const codec = codecArgIndex >= 0 ? String(args[codecArgIndex + 1] || "h264").toLowerCase() : "h264";
if (!["h264", "hevc", "h265", "av1"].includes(codec)) {
  console.error(`--codec must be h264, hevc or av1 (got ${codec})`);
  process.exit(2);
}
const wireCodec = codec === "hevc" ? "h265" : codec;  // settings spelling
if (forceRaw && wireCodec !== "h264") {
  console.error("--force-raw is H.264-only: HEVC/AV1 are GPU-direct or refused (spec 2026-09-20 §5)");
  process.exit(2);
}
```
In the `destinationSettings` object add `videoCodec: wireCodec,` and `allowEnhancedRtmp: true,` after `encoderMode: "auto",`. Update the usage comment at the top (`[--codec h264|hevc|av1]`), the "streaming :" console line to include the codec, and the GPU-path assertion to also require the codec:
```js
const tookGpuDirect = !!pathLine && pathLine.includes("path=gpu-direct") && pathLine.includes(`codec=${codec === "h265" ? "hevc" : codec}`);
```
Print the `[gpu-encode] started ... mft=hardware-<codec>` line from the core log next to the path line.

- [ ] **Step 2: Run the gate three times on the owner's rig (real GPU, real FFmpeg)**

```powershell
node scripts/validate-gpu-encode.mjs --seconds 30 --codec h264
node scripts/validate-gpu-encode.mjs --seconds 30 --codec hevc
node scripts/validate-gpu-encode.mjs --seconds 30 --codec av1
node scripts/validate-gpu-encode.mjs --seconds 30 --force-raw
```
Expected: each of the first three prints `encode path   : [gpu-encode] path=gpu-direct codec=<codec> ...`, received fps ≥ 58, sink speed ≥ 0.97x, `GPU-DIRECT ENCODE GATE PASS`. The fourth passes on H.264 as today. Paste the four summary blocks into the commit message.

- [ ] **Step 3: Update CLAUDE.md**

In the "GPU-direct hardware encode for streaming (#521 slice 1, 2026-09-13)" section, add a bullet at the top:

```markdown
- **HEVC AND AV1 RIDE THE SAME PATH (2026-09-20, owner rulings after the YouTube
  "not enough data" incident).** `GpuVideoEncoderConfig::codec` selects the hardware
  MFT (NVIDIA H.264/HEVC/AV1 Encoder MFTs); HEVC is bound with
  `CODECAPI_AVEncMPVDefaultBPictureCount = 0` because the FLV muxer refuses
  reordered raw HEVC ("Packet is missing PTS", measured); bitstream mode names the
  raw demuxer per codec (`-f h264|hevc|obu`) and copies into FLV, where this FFmpeg
  writes the enhanced-RTMP fourcc itself. **A codec the machine or destination
  cannot honor REFUSES the start** (`StreamStartAdmission.h`: `enhanced-rtmp-required`,
  `no-hardware-encoder`, `gpu-encoder-start-failed`) — the 2026-09-20 failure was a
  silent H.265→H.264 downgrade onto the raw path at 0.87x real time. H.264 keeps
  every path it had; HEVC/AV1 are GPU-direct or nothing until the raw fallback is
  made real-time (sub-project 2). The 2026-08-06 HEVC exclusion in
  `EncoderPolicy.h` was reversed by the owner on 2026-09-20 (patent exposure
  accepted). Gate: `node scripts/validate-gpu-encode.mjs --codec h264|hevc|av1`;
  the per-codec real-GPU round-trips in `MediaFoundationGpuVideoEncoderTest` are
  Windows-only and must run on a `COREVIDEO_WITH_MF_ENCODER=ON` build before merge.
  Spec: `docs/superpowers/specs/2026-09-20-gpu-direct-hevc-av1-stream-design.md`.
```
Also fix the stale sentence in that section "the GPU-direct MFT is H.264-only (an enhanced-RTMP HEVC/AV1 stream must not be fed an H.264 bitstream)" wherever it appears in CLAUDE.md or `GpuVideoEncoder.h`.

- [ ] **Step 4: Update the backlog row**

In `docs/BACKLOG.md` on the #521 row, append: "**2026-09-20:** HEVC/AV1 on GPU-direct + refuse-never-downgrade shipped (sub-project 1 of the 2026-09-20 spec); sub-project 2 (raw fallback at real time) and sub-project 3 (recording/ISO on the seam, absorbs #525) are next and need their own specs."

- [ ] **Step 5: Commit**

```bash
git add scripts/validate-gpu-encode.mjs CLAUDE.md docs/BACKLOG.md
git commit -m "Gate per codec (--codec), CLAUDE.md + backlog for GPU-direct HEVC/AV1"
```

---

### Task 11: Live acceptance on YouTube (owner-present, manual)

**Files:** none (evidence goes in the PR description).

- [ ] **Step 1: Owner sets Stream codec = H.265, Enhanced RTMP on, starts the stream to YouTube.** Confirm from `%LOCALAPPDATA%\CoreVideoPro\media-core.log`: `[gpu-encode] path=gpu-direct codec=hevc` and `started 1920x1080@60 ... mft=hardware-hevc`. From the newest `%LOCALAPPDATA%\Temp\corevideo-ffmpeg-rtmp-*.log`: `fps= 60 ... speed=1.0x` sustained for 5 minutes with no "Resumed reading ... after a lag" lines. YouTube Studio ingest health: green. `nvidia-smi --query-gpu=utilization.encoder --format=csv`: non-trivial.
- [ ] **Step 2: Same with AV1.**
- [ ] **Step 3: Same with H.264** (regression: nothing changed for it).
- [ ] **Step 4: Negative: Enhanced RTMP OFF + H.265.** The stream must NOT start; the transport status reads the Enhanced RTMP sentence; `media-core.log` shows `stream start REFUSED code=enhanced-rtmp-required`.
- [ ] **Step 5: Record all five results in the PR, then open the PR** with `superpowers:finishing-a-development-branch`.

---

## Self-review

- **Spec coverage:** §1 seam → Task 1; §2 MFT per codec + HEVC B-frames off → Task 7; §3 demuxer per codec → Task 4; §4 path decision on sent codec, `codec-not-h264` retired → Tasks 1, 8; §5 refusal (compat, policy reversal, encoder start) → Tasks 2, 3, 6, 8; §6 probe AV1 → Task 5; §7 shell sentences + log line → Tasks 8, 9; §8 tests + gate + manual acceptance → Tasks 1-7 tests, 10, 11; §9 out-of-scope untouched.
- **Placeholders:** none; every code step carries its code. The one open lookup (Task 9 Step 3: which prefix `NormalizeStreamingFailureDetail` leaves) is stated as a read-first instruction with the fallback.
- **Type consistency:** `GpuVideoEncoderConfig::codec` (Task 1) is read in Tasks 7 and 8; `chooseStreamEncodePath(base, codec, codecHasGpuEncoder, frameHasEncoderTexture, reason)` matches between Task 1 and Task 8; `RtmpCompatibilityResult::refused/reason` (Task 2) feed `StreamStartAdmissionInputs::compatibilityRefused/compatibilityReason` (Tasks 6, 8); `videoBitstreamCodec` (Task 4) is set in Task 8; `canonicalProbeCodec` (Task 5) is used in Task 8; `GpuVideoEncoder::lastFailure()` is declared in Task 8 Step 4 and implemented in the MF impl there (Task 7's `fail()` is what it exposes).
