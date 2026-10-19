# Graphics show templates

Graphics starts with four coordinated examples: Interview, Panel discussion,
Webinar and Simple branded show. They use the shipped native template catalog;
they are Graphics sets, not replacements for scenes, audio or output settings.
Each set contains a guest introduction, welcome title, show logo and live clock.
The card images are bundled reference PNGs rendered by the native typography
path from the managed starter drafts, including each set's actual colors. They
remain visible before the core connects; they are not a shell production renderer.

## Operator workflow

Choose **Try this template** to open a populated private sample. Select one of
the four sample graphics at the left, change its Content or Style, and see the
native authoring preview. Changes are retained while switching samples. The
shared show title and brand color can be applied to the set. **Use my logo**
validates a local PNG/JPEG through the existing immutable image-package loader,
uses it for the logo graphic and references its verified bytes in the compatible
lower third. No Program, output or automation command is sent by browsing.

**Customize layout & motion** exposes Layout, Motion and the existing layer
list. Simple controls keep Content and Style available. Individual templates,
saved cues, styles, packages, the layer editor, automation and existing overlays
remain reachable through their existing drawers. Source-name binding is an
optional expander rather than the first expanded control.

**Add this show template** adds four named, global, hidden cues, saves their
drafts and exact template/image pins in the separate Graphics document, and
selects the guest introduction. Nothing is cued, shown, assigned to Studio or
armed automatically. Cue/Show/Hide/Update remain explicit. **Use in Studio**
assigns the guest introduction to the existing Studio lower-thirds toggle.
Cue labels include the show-template name (for example, `Interview · Guest
introduction`), which also carries through to Studio's assigned-look label.
The disabled assignment button explains when adding/selecting a lower third is
required. Studio's layout is unchanged.

**Continue my show** returns to active cues. When no active cues exist, the
operator's click restores saved, unbound cues hidden. Bound cues require a current
guest identity; sets exceeding eight native slots require an explicit selection
in the saved-cue drawer. Loading a document alone never restores or shows cues.

## Admission and failure behavior

A complete set needs four free native instance slots. Existing/on-air instances
are retained, never evicted. Missing or changed template pins are refused before
creation. Native admission remains authoritative if capacity changes during the
operation. A partial refusal reports how many hidden cues were added; it never
retries or rolls back another client's work. Saving names only the operation's
returned instance IDs. Unsaved scratch layer edits must be saved as a template
version before adoption. A media-core epoch change invalidates the private set.
Persistence failures report that the added hidden cues still need saving.

## Preview wire contract and verification

Graphics uses the native private appearance canvas (`preview:lower-third`). Its
`LowerThirdPreview` carries `graphics` and **explicit `appearance:null`**. The
transport normally omits nulls, but this one discriminator must remain present:
the native controller accepts null and refuses a missing appearance field. The
model overrides null omission only for that property. Native identity/revision
and asset validation remain unchanged.

Local image manifests also include explicit no-animation defaults. The native
loader requires the `animations` section even for a static logo; staging bytes
alone is not evidence that an image package was admitted (#949).

`scripts/qa/graphics-starter-probe` runs the managed drafts against a separate
native stdin/stdout process. It checks all sixteen sample surfaces, adds each set
hidden, saves exact pins, removes/restores saved cues hidden and never starts an
output, camera or meeting. The focused D3D RPC regression reads actual shared
texture pixels for compatibility and declarative drafts and verifies text edits
change pixels without creating Program instances.

The compiled `--verify-graphics-workspace` probe checks the four-card starting
screen, private sample navigation, bounded 1080p controls and existing Studio
assignment. Its optional `COREVIDEO_GRAPHICS_PROBE_CATALOG` accepts a catalog
collected by the separate native probe for real native thumbnail bytes. RenderTargetBitmap
screenshots do not capture SwapChainPanel video. Neither these tests nor ready
surface counters establish physical-display continuity or output performance.

For reference-asset regeneration, the managed native probe writes
`starter-thumbnails.json` beside its report. Set `COREVIDEO_GRAPHICS_STARTER_FIXTURES`
to that file and `COREVIDEO_GRAPHICS_STARTER_ASSET_DIR` to the repository's
`native-shell/CoreVideoPro.WinUI/Assets/GraphicsStarters` directory, then run the
focused `GraphicsRpcPixel.PrivateGraphicsPreviewRequiresExplicitNullAppearanceAndProducesChangingPixels`
Release test. It renders the four reference PNGs within the existing bounded
thumbnail limits and still runs its unconditional RPC/shared-pixel assertions.
