# First-click Join binding — #587

The meeting field previously used WinUI's default focus-loss update. Its visible
text could therefore differ from SettingsViewModel.JoinMeetingUrl when Join ran.
The compiled two-way binding now uses PropertyChanged, committing an ID, pasted
URL or invalid edit before the next command, without needing navigation or blur.

`--verify-operator-settings <report.json>` opens only an owned offscreen Settings
window. It does not start a core, join a meeting or register app activation. The
probe edits the real compiled TextBox while it retains focus, checks the model
immediately, and invokes the actual Join command on an invalid synthetic link.
That command must validate the currently visible value rather than a stale valid
ID. Reports contain no real meeting URL/password.

Local evidence in `operator-ux-587/artifacts/`: the same probe with the original
binding fails (`join-before-probe.json`, retained). With PropertyChanged it passes
(`join-window-probe.json`). Release WinUI build passes. This proves the field and
command boundary on the local Windows/XAML runtime; no real Zoom join or sustained
meeting acceptance is claimed.
