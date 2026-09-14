# Audio-meter lifetime hardening — 2026-09-14

Issue: [#513](https://github.com/iamfatness/CoreVideoPro/issues/513).

## Evidence boundary

The original idle crash released a native Border from a WinRT wrapper on the GC
finalizer thread. The [owner's follow-up](https://github.com/iamfatness/CoreVideoPro/issues/513#issuecomment-5652789273)
explicitly retracts the proposed immediate forced-GC reproduction: multiple
collections, Takes, and recording cycles survived, and thousands of wrappers
finalized normally. Finalizer-thread release alone is not a bug. The faulting
object's earlier history and the framework release-order failure remain unknown.

This change is mitigation and regression prevention, not a claim that #513 is
fixed. No framework package versions changed. Other dynamic control factories
remain outside this patch.

## Concrete changes

Previously AudioLevelMeter.RenderSegments created a new StackPanel, every segment
Border, and optional scale Grid/Canvas/ticks/labels, then cleared RootGrid. Both
level updates and the 33 ms decay timer used that path.

The host, panel, and scale now persist. The segment pool grows only to its maximum
requested size (48), and the scale pool only to seven marks. Smaller layouts
collapse surplus entries, preserving their identity for later reuse. Steady level
updates change brushes; geometry and scale text update only when layout changes.
Calibration, fit, peak hold, release ballistics, and muted-input coloring are retained.

Level updates cannot start a timer while unloaded. Unload stops the timer,
unsubscribes its handler, and releases the timer reference; load refreshes the
level and resumes ballistics using a fresh timer when needed.

## Validation

- Release WinUI shell build: passed, zero errors (existing warnings remain).
- Existing WinUI suite: 1,477 passed, zero failed/skipped.
- New standalone WinUI lifetime test: passed 10,000 updates, changing orientation,
  level, mute, scale visibility, segment count, and size. All 48 segment and 14
  scale-child identities remain stable. Checks responsive scale labels, segment
  fit, normal/muted/pre-mute coloring, hidden scale, and unloaded timer suppression.
- Full collection/finalization runs off the UI thread while the dispatcher stays
  available; the retained control remains usable afterward.

The executable compiles the production control and XAML into an isolated WinUI
application with no window. It starts no media engine or control API and writes no
operator settings. It exercises real XAML objects and layout; it does not simulate
Loaded/Unloaded by attaching a window. Live page navigation and the original idle
crash still need observation; a passing finite run cannot prove the crash absent.
The regression executable is included in the Windows CI job with a five-minute
timeout so a native hang cannot block the runner indefinitely.

Run locally:

```powershell
dotnet build native-shell/CoreVideoPro.WinUI.LifetimeTests/CoreVideoPro.WinUI.LifetimeTests.csproj -c Release -p:Platform=x64
& ./native-shell/CoreVideoPro.WinUI.LifetimeTests/bin/x64/Release/net9.0-windows10.0.19041.0/win-x64/CoreVideoPro.WinUI.LifetimeTests.exe
```

Next evidence for #513: retain matching binaries and any new idle-crash dump,
compare the failing reference-tracker path and object history, and investigate
other factories independently. Do not close the issue on reduced allocation
pressure or forced-GC survival alone.
