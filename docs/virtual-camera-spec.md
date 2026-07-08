# Virtual Camera — Architecture Spec & Plan

Status: owner-requested 2026-07-07 ("pick up the virtual camera epic"). North-star item:
output CoreVideo Pro's program as a system webcam so it can be selected in Zoom, Teams,
Meet, OBS, Discord — the vMix/Ecamm/mimoLive parity feature that turns a production
studio into "just another camera" everywhere else. Companion: the existing output
senders (RTMP/SRT/NDI) are the model — this is the same "program video → other app"
shape, but the consumer is the OS camera stack instead of a network protocol.

## 1. The platform decision

**Windows 11 Media Foundation Virtual Camera** (`MFCreateVirtualCamera`,
`MFVirtualCameraType_SoftwareCameraSource`, added Win11 22000). This is the modern,
correct path: a registered software camera the OS Frame Server serves to ANY app that
enumerates cameras through MediaCapture/WinRT/DirectShow-bridge — which is what Zoom,
Teams, Meet, and modern apps use. The box is Win 11 (✓ 26200).

Rejected: DirectShow source filter (legacy; many modern apps no longer see DShow-only
cams; per-app quirks). If a Win10 fallback is ever needed it's a separate adapter.

**Process reality (the load-bearing constraint):** the virtual camera's media source
runs INSIDE the Windows Frame Server process (`frameserver.exe`), NOT inside
`corevideo-native.exe`. So our program frames must cross a process boundary to reach it.
We reuse the app's proven cross-process pattern: a **named shared-memory ring** (the same
family as the Zoom/capture SHM). A small **COM in-proc DLL** (`corevideo-virtualcam.dll`),
loaded by the Frame Server, implements the MF media source and reads frames from that SHM.

```
corevideo-native.exe (our process)          frameserver.exe (OS)
  compositor -> ProgramFrame (BGRA)            corevideo-virtualcam.dll (our COM DLL)
  VirtualCameraOutputAdapter                     IMFMediaSource / frame provider
    -> publish frame to  ── named SHM ring ──→     reads latest frame, serves samples
       (NV12, seqlock, latest-wins)                (format-negotiated 720p/1080p @30/60)
  MFCreateVirtualCamera(...).Start() on enable  consuming app (Zoom) sees "CoreVideo Pro Camera"
```

## 2. Boundary shape (seven principles)

Renderer stays dumb: a toggle (`transport.virtualcam.set {on}`) + read model
(`virtualCamera: {status, deviceName, resolution, fps, consumers?}`). The CORE owns the
SHM publish + MFCreateVirtualCamera lifecycle. Mock-first: the stub core simulates the
virtual camera (status flips on/off, no real registration) so shell UX and tests need no
OS camera stack. Health as data: `"off" | "starting" | "live" | "failed"` + warnings
(e.g. "Virtual camera needs Windows 11 22000+"). Safety posture: the COM DLL only serves
frames from OUR SHM (no arbitrary input); registration is explicit operator intent.

## 3. Frame path

- The program frame already exists (`lastProgramFrame_`, BGRA). The adapter converts to
  **NV12** (the camera-native format apps expect; MF/webcam pipelines prefer it) once per
  tick and writes it into the SHM ring with a seqlock (latest-wins; the DLL always reads
  the newest complete frame — a dropped frame is fine, a torn frame is not: the seqlock
  law from the audio ring applies).
- Default output **1280×720 @30fps** (universal, low overhead); negotiable up to
  1920×1080 @60. The DLL declares the format(s); the Frame Server picks per consumer.
- **Disabled / no-program slate**: when the operator hasn't gone live, the camera serves
  a branded "CoreVideo Pro — standby" slate (never a black or frozen frame — a dead cam
  reads as broken). This mirrors the capture-source slate discipline.

## 4. Laws

1. The SHM writer is seqlock'd; the reader never sees a torn frame (audio-ring law).
2. The camera always serves SOMETHING at the declared fps (slate when idle) — a starved
   virtual camera makes the whole app look broken in the consuming app.
3. Registration/unregistration is idempotent and reversible; disable fully removes the
   camera (MFVirtualCamera::Remove) so it never lingers after the app quits (teardown on
   shutdown, like the engine-off rules).
4. No pixel work on the audio worker; the NV12 convert rides the existing output/sender
   cadence (the no-pixel-work-under-locks law).
5. Everything logged (enable/register/first-consumer/disable) — a virtual camera is
   invisible in our own UI once a consumer grabs it; logs are the only visibility.

## 5. Slices (each its own PR)

- **V1 — program-frame SHM publisher + control surface (SAFE FOUNDATION, this first).**
  `VirtualCameraOutputAdapter` (core): NV12 convert + named-SHM seqlock ring publish of
  the program frame, gated by `transport.virtualcam.set`. Snapshot `virtualCamera` read
  model. Stub simulation. Pure NV12-convert + ring tests. No OS registration yet — the
  frame pipe exists and is unit-verifiable; nothing user-visible as a camera.
- **V2 — the COM DLL + MFCreateVirtualCamera (the big one).**
  `corevideo-virtualcam.dll`: IMFMediaSource serving NV12 samples from the SHM ring at
  the declared format; MFCreateVirtualCamera registration on enable, Remove on disable.
  This is where "CoreVideo Pro Camera" appears in Zoom.
- **V3 — format/quality**: 1080p60 negotiation, mirror toggle, the standby slate, aspect
  handling.
- **V4 — shell UI + diagnostics**: enable toggle + status pill in the transport/output
  area ("Virtual camera: live"), device-name setting, consumer count if available.
- **V5 — polish**: Win10 DShow fallback assessment; audio-to-virtual-mic (separate, large,
  its own decision — most virtual-cam workflows route audio via the app's own devices).

V1 lands safe and testable now. V2 is the multi-session native core of the epic and
warrants careful review (COM registration, Frame Server lifecycle, admin/manifest needs).
