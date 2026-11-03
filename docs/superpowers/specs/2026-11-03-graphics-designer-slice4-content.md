# Graphics Designer Slice 4 content specification

This slice implements Phase 2 §6 and Acceptance 4 on top of Slice 3. Existing documents keep their appearance, geometry and motion. New content must survive Save to playlist, a core restart and Operate Show.

## Shapes and text

Ellipse uses the existing layer box and fill model. Line connects two corners of its box, with an explicit diagonal choice, stroke colour and stroke width. Rectangles and ellipses accept optional inset strokes, so strokes fit the authored box and its motion mask. Stroke widths use reference pixels. The Designer exposes these shapes through Add Shape and the Layer inspector.

Text supports an optional outline up to 8 reference pixels and a shadow with colour, horizontal and vertical offsets, and blur up to 16 reference pixels. Draw shadow, outline and fill in that order. Native preparation owns text measurement and rendering for thumbnails, private preview and Program.

## Content width

Groups and plates can opt into auto width with explicit positive minimum and maximum widths. Measure with the same fonts and resolved fields used to draw. A plate follows its containing group's measured content, or the other root content if it is at the root. Exclude the plate itself from measurement and reject cyclic sizing dependencies. Text with a minimum font size shrinks to fit before applying its existing overflow policy. Fixed-size designs use the existing path.

## Images

PNG and JPEG import into the active design and select a new image layer. SVG is rasterised to transparent PNG at twice the layer dimensions with a 4096 px ceiling, preserving aspect ratio. SVG import rejects external references and active content. The logo chooser advertises SVG through the same supported import route. Preserve imported assets through private preview, versioning, reload and reinstall. Reclaim only assets unreferenced by a saved version, playlist pin or active draft. Report rejected imports without changing the draft.

## Acceptance

Import a transparent SVG logo into a lower third. Inspect it on the private canvas, save to playlist, reopen the design and Show from Operate. Verify transparent edges and the expected raster dimensions at 1080p. Include automated checks for legacy pixels, invalid imports, sizing, and the coexistence of new layers with Slice 3 motion.
