# Audio-meter crash hardening and event validation

## Failure and fix

The September 15, 2026 crash of installed build `aac2d98` occurred in
`WinRT.IObjectReference.Finalize` → `ctl::ComObject<DirectUI::Border>::Release`.
The stowed error was `0x8000000e`: the border had already been queued for
UI-affine final release (`m_ignoreReleases = 1`). Its size, rounded corners,
and detached Border → vertical StackPanel → Grid hierarchy match an old
audio-meter segment. The dump establishes the invalid final release; it
does not identify every earlier reference-count operation.

The meter used to create and discard its segment tree on changing levels,
including a 33 ms decay timer. It now creates a bounded visual pool once:
48 segments, eight scale ticks and eight labels. Level changes update brushes;
resizing, orientation, segment count and scale changes reuse the same controls.
There is no Children.Clear/reparent operation in the update path.

The timer only runs while the meter is loaded and its level or held peak needs
animation. Unload stops it, unsubscribes the handler and releases the timer.
Reload starts from the current telemetry. Muting output clears the level and
peak immediately; muted input monitoring keeps its distinct color.
Ballistics use monotonic elapsed time, and invalid telemetry is sanitized.

The adjacent multiview meters already update existing fill rectangles. Their
clock now also refuses to restart from binding changes while unloaded.
Participant audio rows already update in place when channel IDs are stable.
The separate Zoom mute patch keeps roster telemetry current before the
structural refresh gate, including the latest pending coalesced snapshot.

## Repeatable verification

Build/test the real shell (requires the Windows WinUI toolchain):

```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -c Release -p:Platform=x64 -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true
```

Run the real-XAML stress gate, using that build:

```powershell
./scripts/test-audio-meter-stability.ps1 -Seconds 300
# Longer release rehearsal:
./scripts/test-audio-meter-stability.ps1 -Seconds 3600
```

`-Executable` can point to a packaged candidate to test its actual runtime.
The runner launches an isolated off-screen window without activation; it does
not instantiate MainWindow, connect to Zoom/native media, or load/save a show.
The production executable only enters this path with the explicit
`--verify-audio-meters SECONDS REPORT` arguments.

The probe checks 16 meters under changing levels, output/input mute, resizing,
orientation/scale changes, NaN input, and repeated unload/reload. It forces
compacting GC on a worker while the real UI dispatcher runs. It asserts:

- Every retained visual keeps its identity and the pool size stays bounded.
- Output mute is dark; pre-mute input has a different color.
- Unloaded meters cannot restart timers; silence eventually stops animation.
- Twenty separately created/destroyed meters become collectible after unload.
- Native fail-fast, nonzero exit, timeout, missing report or a failed assertion
  all fail the gate. An existing report cannot be reused as evidence.

The ordinary unit suite covers ballistics/peak timing, finite input handling,
compact layout, repeated mute/unmute, and mixer mute preservation.

## Eight-guest live-event release gate

### Local validation completed September 15, 2026

- Release WinUI build succeeded; all 1,493 WinUI unit tests passed.
- The five-minute real-XAML probe passed: 9,377 update cycles across 16 meters,
  156 unload/reload cycles, 1,164 forced collections, and 85,289 observations
  of active animation. All 20 destroyed test meters became collectible.
- No retained meter visuals were replaced during the tested level/layout
  changes. Post-silence and post-unload timers were stopped.
- Test output: `artifacts/test-results/meter-stress-300s.json` and
  `artifacts/test-results/stability-tests-final.log`.
- Tested shell DLL SHA-256:
  `549528C1933DD234C2B9CE56B570638EF7159FE0E30B3EFBF6B127D0EA1CD8F4`.

This is local regression evidence. The fixed build has not been installed into
the operator's application, and the full eight-guest event rehearsal below has
not been run.

The requested target is eight Zoom guests. Event duration and simultaneous
outputs have not yet been specified. Pending those details, use a conservative
rehearsal of at least two hours at the intended output resolution/frame rate,
with program recording and a private test stream. Enable only the real show's
additional ISO, virtual camera, plug-in and monitor configuration, and record
the exact configuration with the result. These are test requirements, not
claims that this rehearsal has already passed.

1. Use a packaged candidate containing this fix and record its build identity,
   machine, driver/runtime versions, participants and output configuration.
2. Keep eight guests connected with active video/audio. Exercise guest
   mute/unmute, camera off/on, leave/rejoin and screen sharing repeatedly.
   Check that source mute and the operator's mixer mute remain distinct.
3. Operate the show: scene takes, Tiles layout changes, mixer controls,
   monitor/bus routing, repeated page navigation, resize/maximize/restore,
   and the plug-ins actually used for the event.
4. Run simultaneous configured outputs for the full rehearsal. Verify recorded
   files with playback and probe their timestamps/duration; inspect the stream
   receiver for uninterrupted audio/video and sync. Exercise supported
   start/stop/restart paths and a controlled network interruption.
5. Capture crash/hang records, memory trend after warmup, frame/encoder drops,
   audio underruns, output faults and UI responsiveness. A crash, hang, lost
   output, silent audio, growing retained controls, or unrecovered fault blocks
   release. Investigate any degradation rather than treating survival as pass.
6. Close cleanly, relaunch, restore the show, and repeat essential routing and
   output checks. Preserve diagnostics and playback evidence with the build.

The isolated meter gate cannot establish Zoom, GPU, plug-in, encoder, recording
or network reliability. Do not label the complete application production-validated
until the live workload above has passed on the intended event hardware.
