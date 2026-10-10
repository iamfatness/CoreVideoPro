# Operator adjustment controls

Design contract for [#848](https://github.com/iamfatness/CoreVideoPro/issues/848),
including lower-third appearance #847, grading #835 and scene setup #591.
Work order remains in `docs/BACKLOG.md`.

The owner prefers sliders for adjustment, with exact numeric entry when the
operator knows a value. A continuous setting uses one synchronized slider/value
pair. The number is another way to edit the same setting, not an independent
override or a mode that disconnects the slider.

## Shared presentation

Each adjustment has a plain-language label, an editable value with its unit,
a suitably wide slider, and an accessible Reset action. Put related controls in
small labeled groups, align their value fields, and use consistent spacing and
type hierarchy. Keep frequent controls visible; disclose uncommon advanced
settings without hiding current values or active processing.

Show significant endpoints and a neutral/default tick where meaningful. Short
help describes the effect rather than repeating the parameter name. For example,
lower-third background opacity shows `80%` and explains that higher values hide
more of the picture behind the text. Do not expose internal normalized values
where a truthful percentage or physical unit is available.

Use native Fluent controls and normal keyboard/focus semantics. Provide enough
contrast and pointer target area at high DPI. Color alone must not communicate
selection, clipping, neutral state, or whether a processor is enabled.

## Interaction and values

- Dragging updates the displayed value and the permitted live or draft preview.
  Arrow keys provide small steps; larger steps are documented and consistent.
- Clicking the number permits precision entry. Enter commits a valid value;
  Escape restores the value before that text edit. Invalid/non-finite/out-of-domain
  input shows an inline explanation and leaves the underlying setting intact.
- Both controls share one canonical value. Typing a value between slider steps
  retains its precision; a binding refresh must not round it to the nearest step.
  Display rounding does not rewrite the saved value.
- A slider's convenient adjustment range must not silently clamp an existing or
  typed valid native value. Adapt the displayed range or explicitly indicate a
  supported value outside the normal adjustment range.
- Reset restores the documented default; it is keyboard accessible. A drag or
  committed number edit forms one undo action where that workflow supports undo.
- Scrolling the page over an unfocused control does not change a setting.
- Live/draft behavior is visible. Preserve current audio live operation and the
  grade editor's explicit live-edit choice. Lower-third appearance drafts remain
  drafts until Apply. Never add an Apply requirement silently to a live control.
- Keep feedback immediate while bounding native updates: coalesce intermediate
  adjustments, retain the latest value and deliver the final committed value.
  Do not queue every pointer event or rebuild whole panels on each change.

## Meaningful scales and feedback

| Area | Primary interaction | Precision and feedback |
|---|---|---|
| Lower thirds | Sliders for size, opacity, padding, width and offsets; swatches for colors; preset cards | Units tied to output geometry, rendered graphic preview, safe-area guides and original/edited comparison |
| Scenes | Existing canvas drag/resize, alignment and fit/crop actions; sliders for scalar adjustments | Synchronized position, size and crop fields; aspect-lock state and source/scene preview |
| Audio | Retain mixer faders; slider/value pairs for processing and pan | dB, Hz, ms and ratio labels; logarithmic frequency where appropriate; meters/response tied to the actual processor |
| Grading | Slider/value pairs for continuous primary adjustments; curves and color controls where the task benefits | Native transform units only, explicit neutral values, original/graded picture and scopes; overall intensity displayed as a percentage |

Increasing a slider must consistently increase its named quantity. Explain
conceptually bipolar controls with endpoint labels such as cooler/warmer or
left/right. Do not label normalized temperature as Kelvin or basic exposure as
stops unless the corresponding native transform implements those units. Verify
conversion in both directions against the consumer before migrating a display.

## Verification contract

Review representative rendered controls across all four areas. Verify pointer,
keyboard and precision entry stay synchronized, defaults/reset are correct,
invalid input is recoverable, and existing preferences round-trip unchanged.
Exercise supported values outside ordinary adjustment ranges and rapid drags.
Inspect actual native effects and final-value delivery, including Apply/Cancel
and source switching. Accessibility, narrow windows and high DPI are part of
the control review. Sustained media delivery remains separate bake qualification.

## Design references

- [Microsoft slider guidance](https://learn.microsoft.com/en-us/windows/apps/develop/ui/controls/slider):
  relative adjustment, immediate feedback, meaningful labels and ticks, and exact
  numeric entry when precision is needed.
- [Carbon slider guidelines](https://www.carbondesignsystem.com/building-blocks/core/components/slider/guidelines):
  bounded visual adjustment and consistent slider structure.
- [Windows design basics](https://learn.microsoft.com/en-us/windows/apps/design/basics/):
  control grouping, spacing and visual hierarchy.

The interaction and validation decisions above are CoreVideo's design choices;
the referenced systems inform them rather than supplying product requirements.
