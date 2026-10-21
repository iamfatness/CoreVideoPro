# Graphics operator workflow

This is the October 20 #978 source audit and interaction contract, based on main
`3a974344`. It complements the [show starters](graphics-show-starters.md) and
[Studio assignment](graphics-workspace-studio-look.md) references. Backlog order
remains in `docs/BACKLOG.md`.

## What already shipped

| Operator task | Existing consumer retained |
|---|---|
| Choose a coordinated show look | Four populated starter sets, each with introduction, welcome title, logo and clock; bundled reference images remain available before core connection. |
| Experiment privately | Native draft preview, per-sample text retention, shared accent/logo, Content/Style and optional Layout/Motion/layers. |
| Assign the look to Studio | `GraphicsStarterWorkflow` adopts hidden cues, saves them, then pins an independent introduction look; the existing Studio LT control reveals it explicitly. |
| Preserve the current broadcast | Assignment never sends Cue/Show, enables outputs or arms automation. Editing another cue does not mutate the pinned Studio copy. |
| Override a guest's text | Source-ID keyed name/secondary overrides persist. Missing secondary override means source default; an explicitly empty string means hidden. Reusing an input slot does not reuse another guest's overrides. |
| Configure scoped introductions | Existing native Manual/Suggest/Rehearsal/Automatic modes, explicit arming, dwell/auto-hide/cooldown, manual holds, Clear inhibition, saved rules and bounded decision trace. |

Open parent issues are not evidence that all their code is absent. This pass does
not rebuild those consumers or change scene/audio/output settings. Footer status
and alignment stay in #954/PR #955.

## Guest text handoff

The lower-third Content inspector explains the text used by **Use in Studio**:

- With **Use each guest's name and title in Studio**, Sources owns the text.
  Private examples help judge the look; they are not a guest rename. **Edit guest
  names in Sources** opens the existing Sources screen without assigning a look
  or showing a graphic. Starter edits are committed before this handoff.
- With that option off, Studio uses the graphic's text. The inspector explicitly
  warns that a sample name remains until edited. Non-lower-third individual
  graphics do not show irrelevant Studio text controls.
- Assigning an ordinary lower-third cue commits pending text before capturing
  its independent Studio copy. The current on-air look remains unchanged until
  the next explicit Show.

Sources' text flyout offers **Source default**, **Custom text**, or **Hide
secondary line**. Hiding/defaulting does not erase the locally typed custom draft;
switching back to Custom restores it before Apply. The preview is a text preview,
not a promise of the final graphic's typography. **Apply guest text** commits both
lines once to the captured source ID. Guest reassignment while the flyout is open
still refuses Apply. Blank custom text retains the existing hidden semantics.

## Automation editing and observed state

The mode selector follows native mode until edited, while the observed-state
line always reports the core. Mode help states the actual Program boundary:
Manual does nothing automatically, Suggest requires operator Cue/Show,
Rehearsal requires Arm and affects Preview only, Automatic can affect Program
after explicit Arm and dwell. Changing mode disarms it; reapplying the same mode
is a native no-op and does not imply disarming.

The drawer follows **Choose behavior → Configure an introduction → Activate when
ready**. Timing, manual overrides/removal, persistence and native trace are
expandable sections; the primary mode, subject, Apply, Arm/Disarm, suggestion and
hide-and-disarm actions remain in the main flow.

Rule and mode drafts have independent edit guards. Routine trace, activation and
unrelated source-text changes do not overwrite pending fields. An external edit
to that same rule/mode or a core reconnect retains the local draft, explains the
conflict and blocks Apply until **Discard edits and reload current settings**.
Target changes require applying/discarding the current rule draft first. A missing
guest retains its exact source ID as **Unavailable guest**, rather than silently
switching to automatic subject choice. A missing cue cannot receive Apply.

Arm requires an applied native Rehearsal/Automatic mode, enabled rules and no
pending edits. The native core still arbitrates qualification and acceptance.
Save writes applied rules, so it and Restore are unavailable while local edits
are pending. Suggestion Cue requires an observed suggestion for that target.
Cue-specific Hold/Resume never falls through to global Hold when no cue exists.
Disarm does not hide an already shown graphic; the explicit hide-and-disarm
action remains available. Request acceptance is separate from native composition
and receiver delivery.

## Verification boundaries

CPU-only tests cover edit-guard conflicts, runtime-only refresh, reconnect, guest
text/default/blank persistence and starter/Studio assignment contracts. Hosted
Windows CI compiles the app and runs the existing compiled Graphics and operator
settings probes, extended to exercise the actual mode selector, absent guest,
pending dwell, conflict/reload and secondary-line mode controls.

The owner reserved the installed app and GPU for #975. No local WinUI/native
media probes, launch, install, setting/profile writes, camera registration or
output tests are part of this pass. Native pixels, installed review and output
delivery acceptance remain unverified by this batch; #927/#593/#592/#929 keep
their independent acceptance requirements.
