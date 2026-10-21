# Graphics workspace and Studio look assignment

The Graphics workspace opens with [populated show templates](graphics-show-starters.md)
for private experimentation, or **Continue my show** for existing cues. Its editor keeps show cues, native authoring preview and the selected
cue inspector side by side. Content, Style, Layout and Motion have independent
scroll areas. Cue, Show, Hide, Update, Cancel and Clear remain in the fixed bottom
bar. Templates, saved cues/packages, automation and existing overlay controls are
available in drawers; they do not extend the main page vertically.

The 1080p/100% layout is checked against an 1840 x 820 usable shell content budget.
The preview preserves its native 16:9 viewport within the remaining space. Help
and native revision diagnostics are optional, while operation failures remain
visible in the action bar.

## Assigning a lower third

The Content inspector explains whether Studio will use Program guest text from
Sources or fixed text from the graphic. Its **Edit guest names in Sources** action
opens the existing Sources page; it neither assigns nor shows a graphic. See the
[operator workflow contract](graphics-operator-workflow.md).

Select a lower-third cue, edit its private draft, then choose **Use in Studio**.
This saves an independent copy of the fields, appearance, placement, motion and
exact template/image pins. Browsing, deleting or editing the original cue does
not change the assignment. Sample text and unsaved template scratch versions
cannot be assigned. This action never cues, shows, changes family authority or
arms automation.

The assigned-look label sits beside Studio's existing lower-third toggle. Its
Show intent prepares and shows the pinned look through native Graphics using a
correlated instance/revision. The API lower-third toggle/set actions use the same
intent. Existing Hide behavior stays in charge of managed lower thirds. If a look
is reassigned while one is on air, the current look stays unchanged; the next
Show uses the replacement.

**Use Program source text in Studio** resolves name/title overrides and a fresh
source identity at Show time. With this off, the copy uses cue text and retains
any explicitly fixed subject by stable source ID. Live process/generation IDs
are never saved. An unavailable subject, changed core epoch/revision, failed
preparation, missing pin or full native instance capacity refuses Show explicitly.
No user cue is evicted to make room.

The assignment is an additive property in the version-2 graphics document, separate
from production output preferences. Startup only loads it; it does not show it.
Manage can remove the assignment without touching Program. Explicitly selecting
legacy lower thirds clears the assignment before changing authority.

## Isolated layout verification

Run the published WinUI app with `--verify-graphics-workspace <absolute-report>`.
The opt-in host uses an offscreen window, an in-memory production store, a unique
graphics document beside its report and synthetic native state. It opens no core,
meeting or camera. It checks bounded actions/canvas, all inspector tabs, preserved
drawers, selection/draft retention and explicit assignment/label updates.

PNG output uses RenderTargetBitmap for control geometry. SwapChainPanel video and
Fluent composition effects are not captured faithfully; these files do not prove
native media pixels, physical display continuity, or output/performance acceptance.
Managed assignment tests separately exercise exact revisions, restart refusal,
source generation freshness, on-air reassignment, persistence and capacity refusal.
