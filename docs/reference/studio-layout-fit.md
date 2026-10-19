# Studio footer fit

The footer has three existing groups: automation/graphics, the fixed transport
group, and output/audio telemetry. Its former Auto-plus-Star grid centered a
fixed 718-pixel transport group in whatever width remained after automation.
That group could neither shrink nor reflow. At a 1360-pixel client width the
compiled view clipped Virtual camera/options and Details and overlapped Take
with the graphics controls. An idle 1904-pixel client fit. Display resolution
alone therefore did not establish whether a window's available content fit.

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
stability at 1904x1000, 1600x900 and 1360x768 with raster-scale evidence. It also
checks Graphics inside the complete shell, private-authoring footer visibility,
and restoration on return to Studio. These are geometry/UI checks, not native
video-presentation or output-performance acceptance.
