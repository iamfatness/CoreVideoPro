# Issue #601: rate-control investigation, 2026-09-24

Rig: NVIDIA RTX 4090, driver 616.92, Windows; Release native tests.
Base checkout: `f814402b`, with pre-existing uncommitted codec-API changes.

## Findings

The original full-range independent RGB noise fixture emits about 59 Mbps at
1920x1080/60 at both 2 and 10 Mbps targets. Explicit CBR/mean/peak codec
properties, moving them before media-type negotiation, reapplying after input
negotiation, and adding a one-second buffer did not resolve this workload.

FFmpeg `trace_headers` on the captured stream showed `pic_init_qp_minus26=0`
and sustained `slice_qp_delta=25`: slice QP 51. This is evidence of quantizer
saturation, not evidence that the configured bitrate never reaches the encoder.

Using the same changing noise pattern with RGB components limited to [96,159]
allows a meaningful rate-control measurement. With explicit codec property calls
disabled (the original media-type-only configuration), the measured rates were:

| Target kbps | Measured kbps |
| --- | --- |
| 2000 | 2001 |
| 6000 | 6034 |
| 10000 | 10473 |

Each measurement excludes 1.5 seconds of startup and counts encoded bytes over
six seconds. Thus the original explanation of #601 is not established. The
configured rate already affects achievable content. This does not establish a
hard ceiling for arbitrary input, network overhead, or instantaneous packets.

The rate conformance fixture now uses reduced-range noise and a +/-10% band.
The original full-range backpressure probe is preserved. Startup failure and
zero output cannot silently pass the conformance test.

## OBS comparison

OBS's native NVIDIA encoder selects NVENC CBR, sets average and maximum bitrate
to the operator target, and configures a one-second VBV buffer. This configures
compression at the selected frame rate; changing frame rate is not its normal
CBR mechanism. Source:
https://github.com/obsproject/obs-studio/blob/master/plugins/obs-nvenc/nvenc.c

A separate FFmpeg native-NVENC experiment on this rig used 640x360 animated
full-range noise scaled to 1080p60, P5/HQ, CBR 2 Mbps, maximum 2 Mbps and buffer
2 Mbit. It produced approximately 22 Mbps for eight seconds. This was a
different noise generator, not an OBS application test or an exact comparison
against the CoreVideo source. It shows that bypassing Media Foundation alone
does not establish a strict ceiling on pathological noise.

## Implementation and validation

The final change explicitly sets CBR or peak-constrained VBR before media-type
negotiation, the target bitrate, and a one-second H.264/HEVC HRD buffer. VBR
uses the existing product ceiling of 1.5 times its target. A driver rejection
of a required rate-control property fails GPU startup with a named reason;
the existing sender handles fallback/refusal. Bitrates that would overflow
the codec API's unsigned bit units are rejected. The sender also honors the
existing 0.5 Mbps product minimum instead of silently imposing 1 Mbps.

The buffer uses bytes, per Microsoft's H.264/HEVC codec documentation, rather
than NVENC's bit units:
https://learn.microsoft.com/en-us/windows/win32/codecapi/avenccommonbuffersize-property

Release hardware tests: eight encoder round-trip/lifecycle/configuration tests
and four rate/backpressure tests passed. The Release stub suite also passed
all 1,193 tests through CTest. For changing, reduced-range noise,
each rate test counts emitted bytes over six seconds following 1.5 seconds of
settling, with +/-10% tolerance and an output cadence check. The operator's
4.5 Mbps minimum was added alongside the issue's original targets.

| Target kbps | H.264 measured kbps | HEVC measured kbps |
| --- | --- | --- |
| 2000 | 2000 | 2002 |
| 4500 | 4487 | 4500 |
| 6000 | 6063 | 6056 |
| 10000 | 10477 | 10419 |

VBR at 6000 kbps (9000 peak) measured 6019 kbps H.264 and 6028 kbps HEVC.
The original full-range noise backpressure probe remains intact: 59535 kbps
at full input cadence, 29745 at half cadence, ratio 0.500. That stress result
is retained as evidence of saturation, not claimed fixed.

`validate-gpu-encode.mjs --check-bitrate` now sets the actual stream profile,
measures video-packet bytes received after startup, and rejects bitrate
overshoot, mismatched encoder configuration, or observed frame shedding.
It excludes container/audio overhead. Simple pictures can undershoot a CBR
target, so this sender gate checks the upper bound; the changing-content
encoder test checks both bounds.

Twenty-second localhost SRT runs with the real sender and fake Zoom source:

| Codec | Target Mbps | Received video Mbps | Received average fps |
| --- | --- | --- | --- |
| H.264 | 4.5 | 3.909 | 60.0 |
| H.264 | 6 | 3.986 | 60.1 |
| H.264 | 10 | 4.069 | 60.0 |
| HEVC | 4.5 | 1.820 | 59.9 |

All four sender runs passed, with one encoder start and zero reported encoder
input shedding. These finite cadence checks are averages, not proof of every
individual frame deadline. They do not replace real-show acceptance.

Reproduction (Release binaries in `native/build-dev`):

```powershell
cmake --build native/build-dev --config Release --target corevideo-native-tests corevideo-native corevideo-zoom-engine-fake
native/build-dev/corevideo-native-tests.exe --gtest_filter=MediaFoundationGpuVideoEncoder.*
native/build-dev/corevideo-native-tests.exe --gtest_filter=StreamBackpressureRateProbe.*
node scripts/validate-gpu-encode.mjs --seconds 20 --bitrate 4.5 --check-bitrate --keep
node scripts/validate-gpu-encode.mjs --seconds 20 --bitrate 6 --check-bitrate --keep
node scripts/validate-gpu-encode.mjs --seconds 20 --bitrate 10 --check-bitrate --keep
node scripts/validate-gpu-encode.mjs --seconds 20 --bitrate 4.5 --codec hevc --check-bitrate --keep
```

## Limits

This is explicit rate-control configuration and measured conformance, not a
transport shaper or a guarantee on arbitrary random noise. Hard-ceiling
behavior under quantizer saturation remains unresolved. No new destination
stop, resolution reduction, frame-rate reduction or release deployment was
introduced. Intel/AMD hardware and real external receivers remain untested.
