# OHG navigation opt-in — #595

Delivery update (October 10): PR #843 is merged and included in installed
`beta-2026-10-10-abb97fb`. The evidence below describes the original development
trial. Remaining installed checks are consolidated in the
[UI/UX acceptance protocol](operator-uiux-installed-acceptance.md); issue acceptance
remains open.

Settings now exposes Enable OHG Show workspace under Optional workspaces.
Fresh/older profiles default off. The additive OhgShowEnabled production
preference round-trips through the existing serializer and store without a
schema migration or changes to the separate OHG show configuration. Downgrading
to a writer that does not know this optional field can drop the opt-in choice;
returning to this build then safely defaults off.

The navigation button uses the preference's visibility. Attempting to select
the hidden tab returns to Settings, including when OHG is disabled while
selected; all other tabs retain their behavior. Turning the option on exposes
the tab without selecting it. This is a UI readiness gate, not a change to OHG
engine commands or live media. The behavior is in a focused StudioViewModel
partial and existing pure tab policy, with only wiring in StudioViewModel.cs.

Local evidence in `operator-ux-587/artifacts/`: Release build through the test
project and 90 focused tests pass (`ohg-settings-tests.log`), covering absent-field
default, enabled/disabled restart serialization, unrelated output preference
preservation, every tab with opt-in on/off, existing OHG page contracts and
operator telemetry placement. The real compiled-XAML probe passes
(`ohg-window-probe.json`) and sees the default-off Settings toggle.

The isolated probe does not construct production StudioViewModel or change the
installed user's preferences. Installed toggle persistence, visual navigation
updates, disabling while selected and confirmation that existing OHG settings
remain available still need the short operator review. They do not require a
long media bake; no installed acceptance or issue closure is claimed yet.
