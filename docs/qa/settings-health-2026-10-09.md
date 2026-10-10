# Settings health navigation — #594

The top navigation now has one Settings destination in place of Zoom. The
separate Diagnose label and Health button are removed. Inside Settings, Health
and support expands the existing diagnostic surface: measured health/status,
verbose diagnostics, engine evidence and support-bundle export remain available.
DiagnosticsView is shared with the existing detached DiagnosticsWindow, so there
is one implementation of its bindings and actions rather than a second status
projection. Live StudioViewModel remains the source for both hosts.

Local Release WinUI build and the owned compiled-XAML Settings probe pass.
`operator-ux-587/artifacts/health-window-probe.json` records expansion with the
shared view and support export button, collapse and a 900×640 window resize. That
probe deliberately does not construct the production StudioViewModel, join Zoom,
export user diagnostics or modify the running installed app. It validates the
compiled control layout; live health refresh, keyboard/operator navigation and
an installed support export still require acceptance. No long bake is needed for
those short checks, but they are not represented as complete here.
