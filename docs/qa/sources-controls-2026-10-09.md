# Sources control audit — #590

The owner requested moving per-guest Role and On dropout into the upper Sources
area before removing the duplicate Zoom feed health panel. This change puts an
Options button beside the source picker on assigned Zoom rows. Its flyout names
the guest and exposes the existing role and dropout commands. Roles remain
meeting-scoped; dropout preferences retain the same canonical `zoom:` key and
storage. An unavailable assigned guest can still edit their saved dropout policy;
role assignment waits for the guest to return.

| Visible control / information | Before | After / operator task |
|---|---|---|
| Ten input assignments, grouped source picker, offline warning | Upper inspector | Retained; discover and assign sources without an automatic replacement |
| In show, display name, input number/type | Upper inspector | Retained; identify and prepare the show |
| Negotiated Format, Rec, state, tally, thumbnail, meter | Existing Inspector projection | Retained unchanged; no second status snapshot |
| Audio pairing, Grade, Unassign | Assigned inspector row | Retained; prepare or detach that input |
| Zoom Role and On dropout | Duplicate lower feed-health list | Guest Options beside the upper source picker |
| Zoom diagnostic summary and recommendations | Sources and Diagnostics | Sources duplicate removed; full feed diagnostics remain in the health view |
| Capture refresh, discovery, signal/state, sync offset, Connect/Take offline | Half-width device section | Retained in a full-width section below the inspector |
| Browser URL and size/add controls | Device section | Retained; create browser inputs |
| SRT add/remove, mode, host, port, latency, stream ID/passphrase | Ingest section | Retained; configure receivers |
| RTMP add/remove, listener URL/runtime state | Ingest section | Retained; configure receivers |
| Preferred Zoom camera max and setup guidance | Inspector header | Retained; maximum-resolution UX remains a separate issue (#681) |

The flyout installs its options and selects a member of that list before attaching
write handlers. It does not use phased SelectedValue bindings inside recycled
rows (the old feed list documented a crash on that path). Every edit checks both
the original editor object and its guest identity against the current button Tag.
Reassignment, unassignment, or container recycling disables stale options and
rejects their write instead of changing another guest. Opening a flyout does not
write preferences. Unknown values remain unselected rather than showing a
different saved value as if it were current.

## Local evidence

Release WinUI build and all 1,607 WinUI tests pass, including the new assignment/
availability test. The existing roster and preferences tests continue to cover
assignment serialization and source-policy storage.

`--verify-operator-settings <report.json>` now additionally tests the actual WinUI
flyout controls in its owned offscreen process: current role/black selection,
no opening write-back, explicit edits reaching only the original guest, rejection
after reassignment, unavailable-guest saved policy, and recycled container rejection
even when the replacement editor has the same guest ID. It materializes the real
compiled Sources ten-row template at window widths 1,100 and 900 pixels with
synthetic Zoom/camera/media assignments, checks Options visibility and row identity,
and verifies layout does not alter those assignments. Six checks pass in
`operator-ux-587/artifacts/sources-layout-window-probe.json`.

This is synthetic XAML and model evidence, not a visual operator acceptance or
live media run. The probe never starts a core or joins a meeting. The installed
user app was preserved.

## Acceptance still required

Use a packaged build with real Zoom/capture/media inputs at the owner's normal
window sizes. Review the relocated controls, ten-row scrolling, capture list and
keyboard use. Change Role and On dropout, verify the production behavior, restart
and confirm source assignments and saved dropout values. The issue stays open
until that acceptance and merge; the weekend bake remains separate.
