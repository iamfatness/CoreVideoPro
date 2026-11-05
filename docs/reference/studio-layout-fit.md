# Studio footer fit

The footer has three existing groups: automation/graphics, the fixed transport
group, and output/audio telemetry. Its former Auto-plus-Star grid centered a
fixed 718-pixel transport group in whatever width remained after automation.
That group could neither shrink nor reflow. At a 1360-pixel client width the
compiled view clipped Virtual camera/options and Details and overlapped Take
with the graphics controls. An idle 1904-pixel client fit. Display resolution
alone therefore did not establish whether a window's available content fit.
`App.BringMainWindowToForeground` explicitly opens a 1440x900 window, even on a
1920x1080 display. Its client width is below the former footer's combined
minimum; it cannot be qualified by checking only a maximized 1080p canvas.

`StudioFooterLayoutPanel` measures the existing groups and puts transport on a
separate row when their combined width does not fit. Wider views retain the
side-by-side arrangement. It does not recreate controls or change commands,
outputs, sources, scene layout, video resolution or display settings.

Automation controls and telemetry cells have bounded footprints. Status changes
and long assigned-look names must not move live transport controls; full health
text remains on tooltips and Details. Primary actions must not require horizontal
scrolling at the tested client sizes.

`--verify-studio-layout <report.json>` creates a separate offscreen compiled
Studio using in-memory production preferences and an isolated Graphics document.
It checks containment, overlap, long labels/statuses and fixed-window control
stability at 1904x1000, 1600x900, a conservative default-startup client budget of
1424x860, and 1360x768 with raster-scale evidence. It also
checks Graphics inside the complete shell, private-authoring footer visibility,
and restoration on return to Studio. These are geometry/UI checks, not native
video-presentation or output-performance acceptance.

`scripts/test-studio-layout.ps1` runs the Studio and Graphics probes against the
prepared build, rejects old/missing/failed reports, and is part of the Windows
CI job. Qualification requires 100% raster scaling and never changes the host's
display settings. The normal Windows results artifact retains reports and PNGs.

## Opaque review images (#1135)

`ReviewImageWriter` captures the compiled XAML subtree, then composites its
premultiplied BGRA pixels over the configured opaque `StudioBackgroundBrush`
before PNG encoding. A subtree can omit the shell behind transparent areas.
The former PNG preserved nearly transparent white Fluent fills and text edges;
a viewer without the Studio backdrop could show white control plates and
jagged text. An opaque capture carries its own backdrop and preserves already
opaque pixels. This changes QA artifacts, not the live control styles.

The existing 750 ms animation-settling delay remains separate from that alpha
boundary. A longer delay is not evidence that an image is readable. The codec
regression decodes the written PNG and checks translucent fills, text edges,
transparent holes and opaque panels against the declared backdrop.
[RenderTargetBitmap pixels](https://learn.microsoft.com/en-us/windows/windows-app-sdk/api/winrt/microsoft.ui.xaml.media.imaging.rendertargetbitmap.getpixelsasync)
· [Encoder pixel/alpha contract](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.imaging.bitmapencoder.setpixeldata)

The Graphics probe retains the initial builder state, then a populated private
fixture at 1360×768 and 1600×900. It captures the inspector at both ends, checks
that its final timing inputs scroll fully into view, and checks the fixed Save
action against the whole captured page rather than only its own container.
A partial field at the edge of a scroll viewport is distinct from a clipped
fixed action. Geometry, PNG opacity and human visual inspection are separate
evidence. Read the fresh screenshots before claiming visual qualification.

RenderTargetBitmap does not qualify SwapChainPanel video, protected content or
native Program delivery. The native pixel/output probes own those paths. A
readable UI PNG is not evidence of a live preview frame or installed behavior.
