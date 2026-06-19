# Alpha preflight

Use this when manual alpha testing is not possible yet, but we still want useful
progress and evidence.

```powershell
npm run alpha:preflight
```

The preflight writes a timestamped report under:

```text
artifacts/alpha-preflight/<timestamp>/alpha-preflight.md
```

## What It Proves

- npm audit has no moderate+ advisories.
- TypeScript protocol/app code typechecks.
- MediaCore C# tests pass.
- Native C++ media-core stub build, tests, and program preview smoke pass.
- WinUI native shell builds.
- Dev native artifacts and staged Zoom runtime are discoverable when present.
- Dev-readiness and record-stream harnesses are attempted and reported as
  warnings when local dev/live prerequisites are missing.

## What It Does Not Prove

- Real Zoom join.
- Raw Zoom participant video/audio entitlement behavior.
- Camera churn, mute/unmute, screen share, and rejoin behavior.
- Real GPU driver, encoder, FFmpeg, or capture-card behavior.

Those still require the home/dev-machine alpha pass.

## Home Alpha Pass

Run the app:

```powershell
npm run run:studio
```

Then validate:

1. Program/Preview status moves from first-frame wait to live/degraded/error.
2. Settings recent-meeting dropdown appears after a successful join.
3. Zoom join, roster, capture toggle, and participant feeds work.
4. Sources can assign Zoom participants and UVC webcam sources.
5. Routing can remove video/audio routes.
6. ISO video destinations allow one source per ISO.
7. ISO audio buses allow one isolated source per ISO bus.
8. Media can import, select, play, and pause assets.
9. Brand Kit can use a selected media asset as the logo and exposes colors,
   lower-third style, caption style, and default overlay behavior.
10. A short recording produces output health and non-zero file bytes.

Attach the latest preflight report plus any support bundle/log paths to the
alpha result.
