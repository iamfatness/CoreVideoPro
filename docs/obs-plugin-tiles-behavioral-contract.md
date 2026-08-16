# CoreVideo OBS Plugin — "CoreVideo Tiles" Behavioral Contract

**Purpose.** A parity target for a from-scratch reimplementation in a D3D11/Metal
C++ compositor. Everything below is extracted from `iamfatness/CoreVideo` at
`origin/main` (`ef5de43`, 2026-08-13). Read-only research; nothing was modified.

**Citation convention.** `path:line` refers to the file **as of `origin/main`**
(`git show origin/main:path`). Line numbers are from that blob, not the working
checkout (which is 421 commits behind).

**Shipped vs. proposed.** Every section marks claims as:
- **[SHIPPED]** — read directly out of compiled source or the `.effect` file.
- **[DESIGN-ONLY]** — appears in a spec but is *not* in the shipped code.
- **[GAP]** — could not be determined from source; stated rather than inferred.

---

## 0. Where the code actually lives

The user's guess of `src/zoom-tiles*.cpp` is wrong. The Tiles OBS source is
implemented in **`src/zoom-supersource.cpp`** (3534 lines, the largest file in
`src/`). The name is historical — the source id is `corevideo_tiles_source`
(`src/zoom-supersource.cpp:109`).

| File | Role |
|---|---|
| `data/effects/corevideo-tiles.effect` | All three shaders: `I420` (tiles), `Solid` (fills), `Glow` (halo) |
| `src/zoom-supersource.cpp` | The OBS source: render path, properties, settings, feed plan, audio hookup |
| `src/zoom-tile-grid.{h,cpp}` | Pure: layout solve, even-snap, cover-crop |
| `src/zoom-tile-crop.h` | Pure: per-slot crop composed with cover-crop |
| `src/zoom-tile-shape.h` | Pure: aspect presets, spacing % → px |
| `src/zoom-tile-border.h` | Pure: border/radius clamp |
| `src/zoom-tile-glow.h` | Pure: glow quad geometry |
| `src/zoom-tile-fill.h` | Pure: membership resolver (Auto/Manual) |
| `src/zoom-tile-slot.h` | Lock-free slot assignment + frame epoch/staleness |
| `src/tile-motion.h` | Pure: critically damped spring, closed form |
| `src/zoom-tile-animator.h` | Pure: animation lifecycle, entry/exit, departures |
| `src/zoom-tile-texture.h` | Pure: texture realloc / upload predicates |
| `src/zoom-tile-retry.h` | Pure: silent-slot resubscribe pacing |
| `src/zoom-tiles-effect{,-policy}.{h,cpp}` | Effect load + fatal/degrading classification |
| `src/zoom-tiles-background.{h,cpp}` | Background source weak-ref + recursion guard |
| `src/zoom-tiles-audio{,-plan}.{h,cpp}` | Per-participant audio scene/group planner + applier |
| `src/tile-clock-probe.{h,cpp}` | **Phase-0 spike only.** Not on the render path. |

**Important architectural note [SHIPPED]:** there is **no presentation clock and
no per-tile sync**. The 2026-08-09 design's central idea — `T = now − L`, per-feed
ring buffers, `frameAt(T)`, audio delay lines — was **never built**. `zoom-tile-sync.*`
and `zoom-tile-buffer.*` do not exist anywhere in the repo (verified by
`git grep` over `origin/main`). The shipped renderer takes each feed's **newest**
frame every graphics tick. `src/tile-clock-probe.*` is the Phase-0 timebase spike
and is referenced only by `tests/tile-clock-probe-test.cpp` and
`CMakeLists.txt:754-762`; the engine merely *logs* `GetTimeStamp()` values
(`engine/src/tile-clock-log.h`, `engine/src/engine-video.cpp:228`). **[DESIGN-ONLY]**
for everything sync-related.

---

## 1. LAYOUT SOLVE

`solve_tile_grid(count, params)` — `src/zoom-tile-grid.cpp:109-177`.

### Inputs

```cpp
struct TileGridParams {           // src/zoom-tile-grid.h:15-21
    double canvas_width  = 1920.0;
    double canvas_height = 1080.0;
    double tile_aspect   = 16.0/9.0;   // width / height
    double gutter        = 8.0;
    double margin        = 8.0;
};
```

### Algorithm [SHIPPED]

```
if count == 0 or tile_aspect <= 0            -> empty
usable_w = canvas_width  - 2*margin
usable_h = canvas_height - 2*margin
if usable_w <= 0 or usable_h <= 0            -> empty

best_tile_w = 0
for rows = 1 .. count:
    cols   = ceil(count / rows)            // (count + rows - 1) / rows
    avail_w = usable_w - gutter*(cols-1)
    avail_h = usable_h - gutter*(rows-1)
    if avail_w <= 0 or avail_h <= 0: continue
    tile_w = min( avail_w / cols,  (avail_h / rows) * tile_aspect )
    if tile_w <= 0: continue
    if tile_w > best_tile_w:                // STRICT >
        best_tile_w = tile_w; best_rows = rows; best_cols = cols
if best_tile_w <= 0                          -> empty
```

**What is optimized:** the **largest tile width**, `src/zoom-tile-grid.cpp:135`.
Since every tile shares one aspect, maximizing width is identical to maximizing
tile *area* — which is how the design states it
(`docs/superpowers/specs/2026-08-09-corevideo-tiles-design.md:78-85`: "keep the `r`
that maximizes tile area"). It is **not** minimizing gaps or leftover slots. A 5-up
at 1920×1080/16:9 therefore lands 3-over-2, not 5-across.

**Tie-break [SHIPPED, subtle]:** the comparison at `:135` is strict `>` and `rows`
ascends from 1, so on an exact tie the **fewest rows** wins. Reimplement with the
same strictness or you will get a different arrangement on ties.

**Note:** the loop tries every `rows` in `1..count` including arrangements whose
`cols` over-commits (e.g. `count=5, rows=4 → cols=2 → 8 slots`). A defensive guard
at `:157` (`if (placed >= count) break;`) stops phantom tiles from such a selection.

### Placement [SHIPPED]

```
tile_h  = best_tile_w / tile_aspect
grid_h  = tile_h*best_rows + gutter*(best_rows - 1)
start_y = (canvas_height - grid_h) / 2                  // :147

for row in 0..best_rows-1:
    placed = row * best_cols
    if placed >= count: break
    in_row = min(best_cols, count - placed)
    row_w   = best_tile_w*in_row + gutter*(in_row - 1)
    start_x = (canvas_width - row_w) / 2                 // :164  — PER ROW
    for col in 0..in_row-1:
        x = start_x + col*(best_tile_w + gutter)
        y = start_y + row*(tile_h + gutter)
        w = best_tile_w ; h = tile_h
```

- **Leftover slots in the last row: CENTERED**, not left-aligned. Each row is
  centered *independently* (`:162-164`), which makes a short final row centered as
  a consequence rather than as a special case. Pinned by
  `tests/tile-grid-test.cpp:77-104` (`test_five_tiles_center_short_row`).
- **Centering is against the full canvas, not the usable box.** `start_x` and
  `start_y` divide by `canvas_width`/`canvas_height`, *not* `usable_w`/`usable_h`.
  The margin therefore only affects **sizing**, never **centering**. With symmetric
  margins these coincide; a reimplementation using `margin + (usable - row_w)/2`
  is equivalent here but would diverge if asymmetric margins were ever added.
- Output is **row-major**, every tile in a row sharing one `y` — the snapper relies
  on that to detect row boundaries (`src/zoom-tile-grid.cpp:59-61`).

### Gutter and margin: what the percentages are OF [SHIPPED]

`resolve_spacing_px(pct_of_height, canvas_height)` — `src/zoom-tile-shape.h:82-89`.

Both gutter and margin are a **percentage of CANVAS HEIGHT** (not width, not the
diagonal, not tile size). Property labels confirm: `"Gap between tiles (% of canvas
height)"` / `"Margin around the wall (% of canvas height)"`
(`data/locale/en-US.ini:106-107`).

```cpp
constexpr double kSpacingDivisor    = 135.0;
constexpr double kDefaultSpacingPct = 100.0 / kSpacingDivisor;   // ≈ 0.7407407…%
// src/zoom-tile-shape.h:31-32

px = canvas_height / (100.0 / pct_of_height);        // :86  — DIVISOR FORM
if (!(canvas_height > 0)) return 0;
if (!(pct_of_height > 0)) return 0;                  // negative/NaN -> 0
if (!(px > 0))            return 0;                  // underflow / inf/inf NaN
return min(px, canvas_height);                       // cap
```

**The divisor form is load-bearing and must be copied verbatim**
(`src/zoom-tile-shape.h:63-73`). Writing `canvas_height * pct / 100.0` is *not*
bit-equivalent: `100.0/135.0` does not round-trip through a multiply and disagrees
in the last bit at hundreds of canvas heights. `100.0 / kDefaultSpacingPct` is
exactly `135.0`, so at the default the expression reduces literally to
`canvas_height / 135.0` = **8 px at 1080p**. Pinned at every even canvas height by
`tests/tile-shape-test.cpp`.

Setting bounds: slider `0.0 .. 10.0` step `0.001`
(`src/zoom-supersource.cpp:3270-3273`, `kMaxSpacingPct = 10.0` at `:60`). The 0.001
step is deliberate — 0.74 rather than 0.741 solves 7.99 px and snaps to a **6 px**
gutter (`:3266-3269`).

### Tile aspect [SHIPPED]

`resolve_tile_aspect(preset, custom_ratio)` — `src/zoom-tile-shape.h:43-60`.

| Preset id (scene file) | Aspect (w/h) |
|---|---|
| 0 `Wide16x9` | 16/9 |
| 1 `Standard4x3` | 4/3 |
| 2 `Photo5x4` | 5/4 |
| 3 `Square1x1` | 1.0 |
| 4 `Portrait3x4` | 3/4 |
| 5 `Tall9x16` | 9/16 |
| 100 `Custom` | `custom_ratio` if `> 0`, else 16/9 |
| anything else | 16/9 |

Custom ratio is clamped to `[0.1, 10.0]` at settings time
(`kMinCustomAspect`/`kMaxCustomAspect`, `src/zoom-supersource.cpp:55-56, 2737-2745`).
The NaN guard is written `!(x > 0.0)`, not `x <= 0.0`, throughout — deliberate, so
NaN from a hand-edited scene file is caught (`src/zoom-tile-shape.h:53`).

**Single-read invariant [SHIPPED]:** the aspect has three consumers (grid solve,
cover-crop, slot-crop). The render path loads the atomic **once** into
`TileGridParams` and every downstream user takes it from there
(`src/zoom-supersource.cpp:1292-1300, 1942`). Reading it twice would let a settings
change mid-frame solve the grid at one shape and sample at another.

### Even-snap pass [SHIPPED — required for I420]

`snap_tile_grid_even(rects, params)` — `src/zoom-tile-grid.cpp:41-87`.

I420 chroma is 2×2 subsampled, so a blit edge on an odd pixel has no valid chroma
sample. **Do not snap each rect independently** — that makes gaps vary by up to 2 px
(`src/zoom-tile-grid.h:52-63`). Instead:

```cpp
even_floor(v) = (v <= 0) ? 0 : uint32(v) & ~1u          // sizes
even_round(v) = (v <= 0) ? 0 : uint32(lround(v/2)) * 2  // origins

tile_w = even_floor(rects[0].width)     // ONE size for the whole wall
tile_h = even_floor(rects[0].height)
if tile_w < 2 || tile_h < 2                       -> empty (all-or-nothing)
canvas_w = even_floor(params.canvas_width)
canvas_h = even_floor(params.canvas_height)
if canvas_w < tile_w || canvas_h < tile_h         -> empty
gutter = even_floor(params.gutter)

// rows detected by a change in y
grid_y = run_origin(rects[0].y, rows, tile_h, gutter, canvas_h)
for each row r:
    row_x = run_origin(rects[first].x, in_row, tile_w, gutter, canvas_w)
    row_y = grid_y + r*(tile_h + gutter)
    tile c: x = row_x + c*(tile_w + gutter), y = row_y, w = tile_w, h = tile_h

run_origin(desired, n, size, gutter, extent):        // :28-37
    run = size*n + gutter*(n-1)
    if run >= extent: return 0
    start = even_round(desired)
    if start + run > extent: start = even_floor(extent - run)
    return start
```

Every gap is identical **by construction** (whole multiples of `size + gutter`), and
an overhanging run is **shifted whole**, never clipped per-cell.

`snap_tile_grid_even` is **all-or-nothing**: it returns one rect per input, or an
empty vector. The render path treats a size mismatch as "unusable" and falls back to
unsnapped targets (`src/zoom-supersource.cpp:1334-1336`).

---

## 2. FILL / CROP — "every tile is filled, never letterboxed"

### Cover-crop [SHIPPED]

`solve_cover_crop(src_w, src_h, dst_aspect)` — `src/zoom-tile-grid.cpp:89-107`.

```cpp
if (src_w <= 0 || src_h <= 0 || dst_aspect <= 0) return {0,0,0,0};
src_aspect = src_w / src_h;
if (src_aspect > dst_aspect) {          // source WIDER than tile
    crop.height = src_h;
    crop.width  = src_h * dst_aspect;   // keep full height, cut the sides
} else {                                // source TALLER (or equal)
    crop.width  = src_w;
    crop.height = src_w / dst_aspect;   // keep full width, cut top/bottom
}
crop.x = (src_w - crop.width)  / 2.0;   // always CENTERED
crop.y = (src_h - crop.height) / 2.0;
```

The result always has `aspect == dst_aspect` exactly and fits inside the source.
Equality (`src_aspect == dst_aspect`) takes the *else* branch — harmless, produces
the identity rect. **Consequence, stated in the design and not a defect:** a 4:3
tile fed by a 16:9 camera crops **more** off the sides
(`docs/superpowers/specs/2026-08-11-corevideo-tiles-gallery-styling-design.md:66-68`;
`src/zoom-supersource.cpp:1926-1927`).

### Composition with per-tile crop [SHIPPED — order is critical]

`solve_slot_crop(src_w, src_h, dst_aspect, crop_left_pct, crop_right_pct)` —
`src/zoom-tile-crop.h:21-52`.

```cpp
constexpr double kMinCropRemainder = 0.1;              // :11

left  = max(crop_left_pct,  0.0) / 100.0;
right = max(crop_right_pct, 0.0) / 100.0;

total     = left + right;
max_total = 1.0 - kMinCropRemainder;                   // 0.9
if (total > max_total && total > 0.0) {                // PROPORTIONAL rescale
    scale = max_total / total;
    left *= scale;  right *= scale;                    // preserves L:R ratio
}

usable_x = src_w * left;
usable_w = src_w * (1.0 - left - right);

cover = solve_cover_crop(usable_w, src_h, dst_aspect); // COVER SECOND
out.x = usable_x + cover.x;                            // translate back
out.y = cover.y;
out.w = cover.width;
out.h = cover.height;
```

**Order is slot-crop FIRST, cover-crop SECOND.** Reversing it "keeps a different
part of the frame — close enough to look plausible on a centred subject, and clearly
wrong on anyone sitting off-centre" (`src/zoom-tile-crop.h:17-20`). Pinned by
`tests/tile-crop-test.cpp`.

Note the slot crop is **horizontal only** (left/right); there is no top/bottom crop
(explicitly out of scope, `…v2-design.md:209`). `y` therefore comes straight from
the cover-crop.

**Bounds:** sliders are `0..45` per side (`kMaxSlotCropPct = 45`,
`src/zoom-supersource.cpp:97, 2637-2646`) — so the sliders can reach at most 90%
total, and `kMinCropRemainder` is a defensive backstop for hand-edited scene files.
With both at 0, `solve_slot_crop` reduces to `solve_cover_crop` **exactly**
(`src/zoom-supersource.cpp:1930-1932`).

### Truncation to whole texels — the sliver bug [SHIPPED, easy to miss]

`src/zoom-supersource.cpp:1944-1990`:

```cpp
crop_x = (uint32_t)crop.x;   crop_y = (uint32_t)crop.y;      // TRUNCATED
crop_w = (uint32_t)crop.width; crop_h = (uint32_t)crop.height;
if (crop_w == 0 || crop_h == 0) -> draw neutral placeholder, return;

// scale MUST divide by the truncated integers, not the doubles
scale = ( r.width / (float)crop_w , r.height / (float)crop_h )
gs_draw_sprite_subregion(tex_y, 0, crop_x, crop_y, crop_w, crop_h);
```

The sprite's geometry is `sub_cx × sub_cy` **in whole texels**. Dividing by the
un-truncated `crop.width` leaves the tile up to a pixel short and exposes a sliver
of neutral canvas along two edges (`:1944-1949`).

The shader's `crop_uv` is derived from the **same truncated integers**:

```cpp
crop_u  = (float)crop_x / tex_w;    crop_v  = (float)crop_y / tex_h;
crop_cu = (float)crop_w / tex_w;    crop_cv = (float)crop_h / tex_h;   // :1975-1978
```

This is exactly why neither the slot crop nor the tile shape changes the border
registration: both move the sub-rectangle, and `crop_uv` is computed *from that same
moved rectangle*. Any deviation misregisters borders on every tile
(`:1959-1974`; pinned in `tests/tile-shape-test.cpp`).

Chroma planes are half-size but sampled with normalized UVs, so **one** sub-region
serves all three planes with no separate math (`:1988-1989`).

---

## 3. BORDERS

### Clamp [SHIPPED]

`clamp_border(width, radius, tile_w, tile_h)` — `src/zoom-tile-border.h:16-25`:

```cpp
limit = min(tile_w, tile_h) / 2.0;
if (limit <= 0.0) return {0, 0};                 // degenerate tile: no border
out.width  = min(max(width,  0.0), limit);
out.radius = min(max(radius, 0.0), limit);
```

That is the "past half the shorter side there is no interior left" rule
(`docs/CORE_PLUGIN_FUNCTIONALITY.md`, Tiles §Borders and glow: lines 58-60 of that
section). It is applied **per tile against that tile's own snapped rect**, not
against the canvas (`src/zoom-supersource.cpp:1892-1898`).

Setting-level bounds applied first at update time: width `0..64`
(`kMaxBorderWidth`), radius `0..128` (`kMaxCornerRadius`)
(`src/zoom-supersource.cpp:64-65, 2667-2676`).

### Inset math [SHIPPED — it is an SDF offset, not a geometric inset]

The border is **not** drawn as a separate stroked rect. It is a second evaluation of
the *same* signed distance field, offset by the border width
(`data/effects/corevideo-tiles.effect:114-119`):

```hlsl
if (border_width > 0.0) {
    // d + border_width is the same rounded rect inset by the border,
    // so the inner corner radius follows the outer one automatically.
    float inner = 1.0 - smoothstep(-aa, aa, d + border_width);
    rgb = lerp(border_color.rgb, rgb, inner);
}
```

So: `inner ≈ 1` deep inside the video → keep `rgb`; `inner ≈ 0` in the border band
(`-border_width < d <= 0`) → replace with `border_color.rgb`. The inner corner
radius is `corner_radius - border_width` **implicitly**, by construction, never
computed.

**The `border_width > 0` guard is behavioral, not an optimization**
(`data/effects/corevideo-tiles.effect:111-113`): without it a zero width would still
tint the outermost pixel row toward `border_color` — "the feature would be visible
while switched off".

Critically, **grid geometry is untouched by borders** — the border insets inside the
tile rect, so a bordered wall has tiles in exactly the same places as an unbordered
one (`…v2-design.md:159-161`; `src/zoom-supersource.cpp:38-41`).

The border also applies to the **no-frame placeholder** — `tiles_draw_neutral` takes
the same `TilePassParams`, so a slot with no frame gets the operator's border at its
own rect rather than inheriting the previous tile's (`src/zoom-supersource.cpp:1059-1077`).

---

## 4. CORNER RADIUS

### It is an SDF alpha mask [SHIPPED]

`data/effects/corevideo-tiles.effect:63-67`:

```hlsl
float rounded_rect_sd(float2 p, float2 half_size, float r)
{
    float2 q = abs(p) - (half_size - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
```

This is Inigo Quilez's standard rounded-box SDF: negative inside, positive outside,
in the same units as `half_size`. `clamp_border()` caps `r` at
`min(half_size.x, half_size.y)`, so `half_size - r` never goes negative and the shape
never inverts; the extreme is a **capsule**, which is a valid rounded rect
(`:56-62`).

### Evaluation in canvas pixels [SHIPPED]

`data/effects/corevideo-tiles.effect:90-109`:

```hlsl
float2 tile_uv = (vert_in.uv - crop_uv.xy) / crop_uv.zw;   // recover 0..1 over tile
float2 p       = (tile_uv - 0.5) * tile_size;              // canvas pixels, centered
float  d       = rounded_rect_sd(p, tile_size * 0.5, corner_radius);

float aa = max(fwidth(d), 0.0001);   // computed OUTSIDE the branches below

float alpha = 1.0;
if (corner_radius > 0.0)
    alpha = 1.0 - smoothstep(-aa, aa, d);
```

Four things a reimplementation must preserve:

1. **`crop_uv` undo is mandatory.** `vert_in.uv` is *not* 0..1 across the tile — the
   video path draws through `gs_draw_sprite_subregion()`, whose quad UVs span the
   crop rect, so the shader must divide it back out. The neutral placeholder path
   draws through `gs_draw_sprite()` (UVs already 0..1) and passes the identity
   `(0,0,1,1)` rather than being special-cased
   (`data/effects/corevideo-tiles.effect:24-35`).
2. **Width and radius are in canvas pixels**, independent of tile size or feed
   resolution — because `p` is scaled by `tile_size` (`:90-91`).
3. **AA is ±1 *screen* pixel** via `fwidth(d)`, so the edge stays one pixel wide
   however the operator has scaled the wall in the scene. `fwidth` is computed
   **outside** the `if` branches so the derivative is never taken in non-uniform
   control flow (`:96-99`) — a correctness requirement on the GPU, not a style
   choice.
4. **`corner_radius == 0` forces `alpha = 1.0`.** Square corners mean the mask is the
   tile rect itself, which the sprite already covers exactly, so the only thing the
   mask could do is soften an edge that is not there. Forced opaque "by construction
   rather than by inspection" (`:102-107`).

### Radius ↔ border interaction

The border reuses `d` shifted by `border_width` (§3), so **the inner radius follows
the outer one automatically**. The rounded mask masks the **video**, not just the
stroke — the background shows through the corner. Rounding only the stroke would
leave square video peeking out behind a rounded outline (`…v2-design.md:163-167`).

### Radius ↔ glow interaction

The glow pass is handed `glow_corner_radius = clamp_border(...).radius` — the tile's
**own clamped** radius, clamped against the tile's snapped rect, not the raw setting
(`src/zoom-supersource.cpp:1806-1822`). The halo therefore follows a rounded tile
rather than hugging a square one (`data/effects/corevideo-tiles.effect:193-195`).

### Blending required by rounded corners [SHIPPED]

`src/zoom-supersource.cpp:1859-1870`:

```cpp
gs_blend_state_push();
gs_enable_blending(true);
gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                           GS_BLEND_ONE,      GS_BLEND_INVSRCALPHA);
```

**Separate alpha factors are load-bearing.** With `SRCALPHA/INVSRCALPHA` for all
four, destination alpha under-accumulates at partial-alpha pixels (e.g. 0.84 instead
of 1.0), visible as a faint translucent ring around rounded corners when the wall is
filtered, nested in another scene, or routed through a colour-space-conversion
texrender. `ONE/INVSRCALPHA` for alpha matches libobs's own `gs_reset_blend_state()`.

---

## 5. GLOW — the full extraction

### It is a separate pass drawn BEFORE the tiles [SHIPPED]

Draw order is stated verbatim in the shader
(`data/effects/corevideo-tiles.effect:159`) and matched by the render path:

> **background colour → background source → glow → tiles**

`src/zoom-supersource.cpp`: background colour fill `:1650-1667`, background source
`:1674`, glow block `:1728-1847`, tiles `:1872-2185`.

**Why separate, both reasons load-bearing** (`data/effects/corevideo-tiles.effect:149-157`,
`src/zoom-tile-glow.h:14-21`):
1. A tile is drawn as a quad exactly its own size, so there is **no canvas outside it
   for a halo to bleed into**. The halo needs a bigger quad of its own.
2. `PSI420` carries the parity-verified BT.709 conversion and the `crop_uv`
   correction — verified twice over and not worth disturbing.

### It is **NOT a blur** — it is an analytic SDF falloff [SHIPPED]

No blur kernel, no ping-pong, no downsample, no texture read at all. The halo is
generated from the distance field alone; the quad is drawn with a **null texture**
(`src/zoom-supersource.cpp:1836-1841`).

### Full shader [SHIPPED]

`data/effects/corevideo-tiles.effect:166-231`:

```hlsl
uniform float4 glow_color;         // only .rgb used; .a deliberately ignored
uniform float2 glow_quad_size;     // expanded quad, canvas pixels
uniform float2 glow_tile_center;   // tile centre, in QUAD-LOCAL pixels
uniform float2 glow_tile_half;     // half the TILE's size — not the quad's
uniform float  glow_corner_radius; // canvas pixels; the tile's clamped radius
uniform float  glow_size;          // falloff distance, canvas pixels; > 0
uniform float  glow_intensity;     // 0..1, alpha at the tile's own edge
uniform float  glow_falloff;       // k, 0..2

float4 PSGlow(VertInOut vert_in) : TARGET
{
    float2 p = vert_in.uv * glow_quad_size - glow_tile_center;
    float  d = rounded_rect_sd(p, glow_tile_half, glow_corner_radius);
    float  t = saturate(d / max(glow_size, 0.0001));

    float ramp    = 1.0 - t;
    float falloff = saturate(ramp * ramp * (1.0 + glow_falloff * t));

    return float4(glow_color.rgb, falloff * glow_intensity);   // STRAIGHT alpha
}
```

**The falloff function, stated plainly:**

```
t = saturate( sd_to_tile_rect / glow_size )        // 0 at the tile edge, 1 at the limit
alpha(t) = saturate( (1 - t)^2 * (1 + k*t) ) * glow_intensity
```

- `k = 0` (softness 0%, the default) ⇒ exactly `(1 - t)^2`.
- `k = 2` (softness 100%) ⇒ `(1-t)^2(1+2t) = 1 - smoothstep(t)` — flat at both ends,
  so the halo holds its strength just outside the tile before falling away through an
  inflection, "which is how a blurred reference reads next to a hard light source".

**Why `(1-t)^2` and why the bound is exactly 2** (`data/effects/corevideo-tiles.effect:201-222`):
- The `(1-t)^2` factor is what stops the halo ending on a visible band. Anything
  smooth multiplied by `(1-t)^2` has **both value and slope 0 at `t = 1`**, so every
  in-range setting keeps that property by construction.
- `d/dt = (1-t)[(k-2) - 3kt]`, which is `<= 0` across `[0,1)` **exactly when
  `k <= 2`, given `k >= 0`**. Above `k = 2` the curve *rises* just outside the tile,
  clips at 1, and draws a bright ring around every tile. (Note the precondition:
  `k = -3` is also `<= 2` but breaks the bound — hence the `>= 0` floor in the
  settings clamp.)

**Operator control → `k` mapping** (`src/zoom-supersource.cpp:1735-1738`):

```cpp
glow_falloff = glow_softness_pct * (2.0f / kMaxGlowSoftness);   // kMaxGlowSoftness = 100
// 0% -> k = 0 ; 100% -> k = 2
```

`glow_softness` is clamped to `[0, 100]` at settings time — **load-bearing, not
defensive** (`src/zoom-supersource.cpp:2708-2717`).

**Peak semantics, worth transcribing** (`src/zoom-supersource.cpp:3327-3333`):
softness does **not** change the peak. The halo is at full intensity **at** the tile
edge for every setting, whereas a Photoshop/Gaussian outer glow sits around half
strength there because half the kernel falls inside the shape. Matching such a
reference means ≈50% intensity plus whatever softness looks right.

`glow_color.a` is deliberately unused — `obs_properties_add_color` exposes no alpha
channel, and halo strength is what the intensity control is for
(`data/effects/corevideo-tiles.effect:226-229`).

### Glow quad geometry [SHIPPED]

`solve_glow_quad(tile_left, tile_top, tile_w, tile_h, glow_size, canvas_w, canvas_h)`
— `src/zoom-tile-glow.h:70-123`:

```cpp
if (!(glow_size > 0.0))                       return {visible=false};  // NaN-safe
if (!(tile_w > 0.0) || !(tile_h > 0.0))       return {visible=false};
if (canvas_w == 0 || canvas_h == 0)           return {visible=false};

left   = floor(tile_left   - glow_size);      // ROUND OUTWARD
top    = floor(tile_top    - glow_size);
right  = ceil (tile_right  + glow_size);
bottom = ceil (tile_bottom + glow_size);

left = max(left, 0); top = max(top, 0);
right = min(right, canvas_w); bottom = min(bottom, canvas_h);
if (!(right > left) || !(bottom > top))       return {visible=false};

q.x = left; q.y = top; q.w = right-left; q.h = bottom-top;
q.half_width  = tile_w * 0.5;                 // the TILE's half, never the quad's
q.half_height = tile_h * 0.5;
q.center_x = (tile_left + q.half_width)  - left;   // absorbs whatever was clipped
q.center_y = (tile_top  + q.half_height) - top;
```

Two non-obvious rules:

- **Round OUTWARD.** The quad is drawn in whole pixels; rounding inward clips the
  last row of falloff and leaves a faint hard edge exactly where the halo should have
  faded to nothing (`:88-90`).
- **`glow_tile_center` is NOT half the quad.** Once the quad is clipped by the canvas
  edge the tile is no longer centred in it; using the quad's centre would slide every
  halo in the outer row and column off its tile by whatever was clipped
  (`data/effects/corevideo-tiles.effect:185-189`, `src/zoom-tile-glow.h:25-29`).
  Pinned by `tests/tile-glow-test.cpp`.

**No clamp on `glow_size` itself** (`src/zoom-tile-glow.h:52-60`,
`src/zoom-supersource.cpp:2688-2694`, spec `…gallery-styling-design.md:39-43`). A glow
wider than half the gutter merges neighbouring halos into a continuous wash; one
wider than the margin clips at the canvas edge. Both legitimate small, obviously
wrong large. The only bound is the slider's own: `0..256` canvas pixels
(`kMaxGlowSize`). Canvas clipping is a consequence of a finite canvas, **not** a
bound on the setting.

### Glow blend state [SHIPPED]

`src/zoom-supersource.cpp:1760-1763`:

```cpp
gs_blend_state_push();
gs_enable_blending(true);
gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                           GS_BLEND_ONE,      GS_BLEND_INVSRCALPHA);
```

**Explicitly NOT additive (`ONE/ONE`)** (`:1754-1756`) — two overlapping halos would
blow out toward white instead of staying the operator's colour, and overlap is
explicitly allowed. Same separate-alpha reasoning as the tile pass. Pushed and popped
so the tile pass starts from exactly the state it always did.

### Skip conditions [SHIPPED]

| Condition | Result | Cite |
|---|---|---|
| `glow_size == 0` | Entire block skipped: no technique, no blend state, no draw. Byte-identical to a wall with no glow. | `:1702-1705` |
| `glow_intensity == 0` | Also skipped — cost saving only; an alpha-0 quad composites unchanged anyway. | `:1699-1701` |
| Effect lacks Glow technique/uniforms | Skipped for the session; the wall still draws; one `LOG_ERROR`. | `:1718-1728` |
| Tile rect `width < 2 || height < 2` | That tile's halo skipped, same rule as the tile loop. | `:1771` |
| `tile_alpha <= 0.0` (entering tile at alpha 0) | Skipped. | `:1787` |
| Quad clamped away to nothing | Skipped. | `:1804` |
| `gs_technique_begin_pass` fails | **`break`**, not `continue` — every tile opens the same pass on the same technique, so a failure fails for all. | `:1827-1832` |

### Glow follows the drawn rect [SHIPPED — commit 9d5cf71, note N2]

The glow pass runs **before** the tile pass, so it must know which rect each tile will
end up at. `solve_glow_quad_for_tile(snapped, composited, moving_xywh, ...)`
(`src/zoom-tile-glow.h:149-160`) selects the snapped rect when `composited == false`
and the true fractional rect when `true`. `can_composite` is resolved **once per
frame, before either pass** (`src/zoom-supersource.cpp:1622-1637`), and a per-tile
render-target failure is **latched** (`ctx->motion_composite_failed`) so the two agree
from the next frame on. Before this fix the halo and tile were solved from different
rects, up to ~2 px apart, on exactly the frames where something had already failed.

The halo also **fades with its tile**: `gp.intensity = glow_intensity * tile_alpha`
(`:1824`). Without it a joining tile draws a full-intensity glow around a still-
transparent rect — a glowing empty rectangle for the whole entry fade
(`:1773-1786`).

---

## 6. ANIMATION

### Easing curve: critically damped spring, exact closed form [SHIPPED]

`spring_advance(s, target, settle_seconds, dt_seconds)` — `src/tile-motion.h:11-52`:

```cpp
if (settle_seconds <= 0.0) { s.position = target; s.velocity = 0.0; return; }
if (dt_seconds     <= 0.0) return;                       // true no-op

constexpr double kSettleFactor = 6.6384;
const double omega = kSettleFactor / settle_seconds;

const double delta = s.position - target;
const double decay = std::exp(-omega * dt_seconds);
const double c     = s.velocity + omega * delta;

s.position = target + (delta + c * dt_seconds) * decay;
s.velocity = (s.velocity - omega * c * dt_seconds) * decay;
```

- **`kSettleFactor = 6.6384` is the critically damped 1%-remaining constant**, the
  root of `(1 + k)·e^-k = 0.01`. It is explicitly **NOT 4.6** — that is the
  first-order constant, and a second-order critically damped system decaying as
  `(1 + ωt)e^-ωt` is still 5.6% short at 4.6 (`src/tile-motion.h:39-43`).
- **Exact solution, not a numerical integrator.** An approximate integrator overshoots
  measurably and by an amount that does not vanish as `dt` shrinks; overshoot means a
  tile sails past its slot and comes back — "precisely the cheap look this feature
  exists to avoid". The closed form also makes results **identical at any frame rate
  by construction** (`:32-37`).
- **Not a cubic ease, deliberately.** A spring re-aims from the current position *at
  the current velocity*; restarting a cubic ease begins at zero velocity, so a moving
  tile would stop dead and re-accelerate — a visible hitch precisely when the wall is
  busiest (`…tiles-animation-design.md:178-186`).
- Four independent springs per tile: `x`, `y`, `w`, `h`
  (`src/zoom-tile-animator.h:277-280`).

### Duration semantics [SHIPPED]

`duration_seconds` is the spring's **settle time** (time to 1% remaining), **and**
the exit-fade duration, **and** the entry-fade duration. One number, three roles.

- Default 350 ms (`src/zoom-supersource.cpp:3099`).
- Slider `100..1000` ms, step 10 (`:3345-3347`).
- `clamp_animate_duration_ms(raw)` clamps `[100, 1000]`
  (`src/zoom-tile-animator.h:531-538`). This was the **only** unclamped setting on the
  source: a negative value from a hand-edited scene file wrapped to 4294967295 ms and
  parked every tile at alpha ≈ 0 forever, wall rendering as background only,
  permanently on the composite path because `settled()` could never become true.
- `duration == 0` (unreachable via the clamp, reachable via the pure API): snap
  immediately, entry fade completes instantly at alpha 1.0
  (`src/tile-motion.h:17-21`, `src/zoom-tile-animator.h:371-375`).

Timing is **elapsed-nanosecond based** from `os_gettime_ns()`, never per-frame
increments (`src/zoom-supersource.cpp:1454`). `dt` is 0 when `!m_has_last` or
`now_ns <= m_last_ns` (`src/zoom-tile-animator.h:74-78`).

**`m_has_last` is an explicit flag, not a sentinel `m_last_ns == 0`** — zero is a
legitimate timestamp, and overloading it forces `dt = 0` on two consecutive calls,
silently losing a frame of motion (`:69-73`).

### "Adoption is its own state" (commit 9d5cf71) [SHIPPED]

Two events that must be told apart, because only one of them fades
(`src/zoom-tile-animator.h:80-101`):

- **ADOPTION** — this animator's **first enabled frame**, whatever it contains,
  including nothing. A scene load, or the operator ticking the Animate checkbox
  mid-show (the disabled bypass discards all state, so the next enabled frame starts
  from nothing). **Fading here would blank a live show for the whole duration over a
  checkbox.**
- **ARRIVAL** — a participant appearing on any **later** frame. That is a join, and it
  fades, because it is drawn at its slot in the **new** grid while every incumbent is
  still at its **old** one.

```cpp
const bool adopting_existing_wall = !m_has_run_enabled_frame;
m_has_run_enabled_frame = true;                      // latched UNCONDITIONALLY
...
m.entering = !adopting_existing_wall;                // :262
```

**The bug 9d5cf71 fixed:** the flag was previously *inferred* from `m_tiles` being
empty. That test conflates "the animator has not run yet" with "the wall happens to
have nobody on it" — so an animator left running on an empty wall (OBS open before the
meeting starts) treated **the first participant ever to appear** as an adoption and
popped them on at full opacity. The commit message records the measurement:

```
empty wall, first participant ever : alpha 0.0000, at_rest, fades in
Animate toggled on over a live wall: alpha 1.0000 on all three tiles,
                                     at_rest, settled, geometry = current slots
```

The commit also records the meta-lesson: *"An empty container standing in for a state
has now been the shape of four separate defects on this branch; this one is a flag."*

`m_has_run_enabled_frame` is reset by the disabled bypass, so the next enabled frame
is an adoption again — "the wall the operator sees when they tick the checkbox is the
wall that stays on screen" (`src/zoom-tile-animator.h:58-61, 404-408`).

### Entry fade [SHIPPED]

`src/zoom-tile-animator.h:249-264, 349-361, 368-383`:

- A newly seen participant is **seeded at its target rect** (`position = target`,
  `velocity = 0`) — it does **not** fly in from an edge or the origin.
- It fades **in place**: `alpha = elapsed / duration_seconds`, linear, over
  `duration_seconds`, retiring `entering` at `elapsed >= duration`.
- **Why:** a join reflows the wall immediately, so on the frame a newcomer appears it
  is at its slot in the NEW grid while every other tile is still at its OLD one.
  Measured at **up to 500,080 px² of overlap on the first frame**, decaying to zero as
  the reflow runs. "The fade is what makes that transient invisible: alpha is ~0
  exactly when the overlap peaks and reaches 1 only once the incumbents have moved
  away." Entering tiles fade in at their final position "so tiles never cross one
  another".
- Nothing is ever withheld — a tile in `desired` is drawn on the frame it appears.

### Exits and why departures reflow immediately [SHIPPED]

**Exits are never drawn.** The design's decision 3 ("a leaving tile holds its last
frame") **was not implemented** (`…tiles-animation-design.md:38-52, 94-96` — marked
"> **Not implemented.**"). A departing tile is removed on the frame it goes absent and
the wall reflows immediately (`docs/CORE_PLUGIN_FUNCTIONALITY.md` Tiles §Animating:
"**A departure reflows immediately** — the leaving tile is not held on screen").

The exit lifecycle and its four invariants **are** implemented and tested — they bound
a real retained capability — but nothing currently reaches air through that path
(`src/zoom-tile-animator.h:139-160`):

1. Only a genuine roster departure may start an exit.
2. A reassignment cuts instantly — no hold, no fade.
3. Any repoint cancels a running exit immediately.
4. An exit can never outlive its duration.

Exit alpha, when it did run: `alpha = 1.0 - elapsed/duration_seconds`, erased at
`elapsed >= duration` (`:191-198`).

**`departed` is a STATE, not an event** — recomputed every frame as "which tracked
participants are gone from the roster right now" (`src/zoom-supersource.cpp:1363-1372`).
`classify_departures()` (`src/zoom-tile-animator.h:503-521`) requires **positive
evidence**: the id must be absent from the layout, absent from the roster, **and**
present in `ever_in_roster`. Without that third test, a Manual-mode cast participant
who never joined the meeting classifies as a departure.

**Documented, unclosed hazard [SHIPPED, note N3 of 9d5cf71]:** an *empty* roster
snapshot classifies **every** participant on the wall as departed — forced in a probe,
that window produced **15,832 exits for participants still in the meeting**. It is
unreachable only because of an invariant *outside* the animator:
`ZoomEngineClient::m_roster` is cleared exactly twice — on `"left"` (a genuine
departure of everyone) and at the top of a `"participants"` rebuild that repopulates it
before releasing the mutex. A disconnect leaves it **stale**, and stale fails safe. A
reimplementation must either keep that invariant or require a non-empty snapshot.

### There is no settle window [SHIPPED — the design's was deleted]

`src/zoom-tile-animator.h:105-137` and `…tiles-animation-design.md:12-36, 197-213`.

The design specified a **250 ms fixed settle window** to absorb roster blips. **It was
deleted.** Holding a change back means a tile's target belongs to the grid generation
current when the hold began; a tile absent at commit time kept a target from an older
generation, and two tiles ended up placed on **two different grids and overlapped
while both were at rest**. Five variants were found and individually fixed; each fix
moved the damage.

The shipped rule is one line, `src/zoom-tile-animator.h:275`:

```cpp
m.target = d.rect;      // UNCONDITIONAL, every tile, every frame
```

With targets refreshed unconditionally there is no second generation for anything to
be on: any two tiles at rest are at their slots in the same solve, which are disjoint
by construction. Verified at **1.44M simulated frames**, including a degenerate
geometry with no gutter.

**Accepted consequence:** a roster blip now reflows the wall out and back. Because the
springs carry velocity through a retarget, the wall turns around from wherever it had
got to instead of stopping and restarting, so a short blip reads as a **wobble** rather
than a pop. A blipped participant is erased and re-created, and so **fades back in**
over `duration_seconds`.

### Disabled is a complete bypass, not a fast setting [SHIPPED]

`src/zoom-tile-animator.h:45-67`:

```cpp
if (!settings.enabled) {
    m_tiles.clear();
    m_last_ns = 0; m_has_last = false; m_has_run_enabled_frame = false;
    // emit `desired` VERBATIM at alpha 1.0, at_rest = true
}
```

Every piece of animation state is **discarded**, not merely left alone — otherwise a
stale in-flight position or half-finished fade would resume the moment the toggle came
back. Clearing here is also what makes `settled()` unconditionally true right after a
disabled call, so the render path treats "disabled" as a *case of* "settled" rather
than a second branch (`src/zoom-supersource.cpp:1498-1518`).

### The even-snapped vs sub-pixel path rule [SHIPPED — the crux]

Documented contract: *"A tile that is not moving draws through the same even-snapped
path it always has, byte for byte. Only a tile actually in motion takes the sub-pixel
path, and it returns to the pixel-exact one as soon as it settles."*
(`docs/CORE_PLUGIN_FUNCTIONALITY.md`, Tiles §Animating).

#### Why snapping exists

I420 chroma is subsampled 2×2, so **a blit edge on an odd pixel has no valid chroma
sample to reconstruct from**. Every tile edge must land on an even pixel
(`src/zoom-tile-grid.h:52-55`, `…tiles-animation-design.md:79-83`).

#### Two mutually exclusive snapping modes, gated by `settled()`

`src/zoom-supersource.cpp:1496-1607`.

**Mode A — wall settled ⇒ whole-grid snap.**
```cpp
rects = snapped;                     // == snap_tile_grid_even(solved, params)
moving.clear();                      // stays EMPTY
```
This is *the exact call the file has always made*, hoisted only so the animator can
target the same numbers. Parity holds by construction, not by the coincidence that the
animator's numbers happen to match.

**Mode B — not settled ⇒ per-tile independent snap.**
```cpp
rects[i] = snap_tile_independently(live->rect);
// src/zoom-supersource.cpp:1131-1139
s.x = even_round_px(r.x);  s.y = even_round_px(r.y);
s.width = even_floor_px(r.width);  s.height = even_floor_px(r.height);
```

**`snap_tile_grid_even()` MUST NOT be used mid-reflow.** It assumes a uniform grid and
derives the single `tile_w`/`tile_h` it uses for **every** row from `rects[0]` alone.
A newly joined tile is seeded at *its* target size while the others are still springing
from the old one, so mid-reflow the animator's tiles legitimately differ in size.
Feeding that list to a function that derives one size from its first element silently
mis-sizes every tile but the first (`src/zoom-supersource.cpp:1456-1470`).

#### The `+0.05` in `even_floor_px` — load-bearing, not defensive

`src/zoom-supersource.cpp:1088-1107`:

```cpp
static uint32_t even_floor_px(double v)
{
    if (v <= 0.0) return 0;
    return static_cast<uint32_t>(v + 0.05) & ~1u;
}
```

The `0.05` is `TileAnimator::kRestEpsilon` exactly. A spring approaches its target
asymptotically **from below**, so a tile that has arrived at an even integer target
sits at `(target − 1e-8)`. A plain floor turns that into `target − 1`, and `& ~1`
drops it a further pixel: a tile that finished growing to 1064 draws **1062** — two
pixels short — for every frame between arriving and the wall becoming `settled()`.
Measured across 2→1 through 12→11 reflows at 1920×1080, that hits **five of the eleven
counts**. Widening the floor by exactly the band the animator calls "arrived" changes
nothing for a tile genuinely still in flight (0.05 px is 1/40th of the 2 px step).

`even_round_px(v) = uint32(lround(v/2.0)) * 2` for **origins** — nearest even, so a
snapped tile stays close to the animator's true position rather than always shifting
toward the origin.

#### Why `settled()` is stricter than `all_of(at_rest)`

`src/zoom-tile-animator.h:329-340`. `settled()` returns false if **any** tracked tile
is `exiting` **or** `entering`, or any spring is `>= kRestEpsilon` from its target.

- An **exiting** tile is not in the layout a fresh solve describes at all.
- An **entering** tile is at its final slot and perfectly still, so it reports
  `at_rest` — but it is part-way through its fade, and the whole-grid branch draws
  through the snapped blit, **which has no alpha**. An `all_of(at_rest)` gate would
  silently drop every entry fade and the tile would pop in at full opacity.

`kRestEpsilon = 0.05` is shared by `at_rest` and `settled()` so the two can never
disagree (`:399`).

#### The sub-pixel composite path

`src/zoom-supersource.cpp:2007-2185`. A tile takes it when
`!live->at_rest || live->alpha < 1.0` (`:1597`) — **moving OR fading**, deliberately
not `at_rest` alone, because an entering tile is perfectly still yet needs an alpha the
direct blit cannot apply.

```
1. gs_technique_end(tech)                       // wall's technique must close:
                                                //   gs_effect_loop() refuses to run
                                                //   while another effect is active
2. gs_texrender_reset(tr)
   gs_texrender_begin(tr, r.width, r.height)    // EVEN dimensions
3. gs_ortho(0, r.width, 0, r.height, -100, 100) // texrender sets viewport but NOT
                                                //   projection; without this the tile
                                                //   is drawn through the CANVAS ortho
                                                //   and comes out shrunk into a corner
4. gs_blend_state_push(); gs_enable_blending(false)   // STRAIGHT alpha in the target
   gs_clear(GS_CLEAR_COLOR, transparent)
5. gs_technique_begin(tech); draw_tile(i, r, 0, 0); gs_technique_end(tech)
6. if (alpha < 1.0) fade_tile_alpha(r.width, r.height, alpha)
7. gs_blend_state_pop(); gs_texrender_end(tr)
8. srgb-aware bind:
     gs_framebuffer_srgb_enabled() ? gs_effect_set_texture_srgb : gs_effect_set_texture
9. gs_matrix_translate3f(moving.x, moving.y, 0)          // FRACTIONAL position
   gs_matrix_scale3f(moving.width / r.width,
                     moving.height / r.height, 1)        // FRACTIONAL size
   while (gs_effect_loop(OBS_EFFECT_DEFAULT, "Draw"))
       gs_draw_sprite(tile_tex, 0, r.width, r.height);
10. gs_technique_begin(tech)
    if (!composited) draw_tile(i, r, r.x, r.y);          // fallback
```

Key invariants for a reimplementation:

- **The tile lives at (0,0) with even dimensions inside the intermediate**, so the
  chroma rule is satisfied in **tile space**; where the finished tile lands on the
  canvas is then free to be fractional. That is the whole trick
  (`src/zoom-supersource.cpp:2018-2025`).
- **The intermediate is `GS_RGBA`, deliberately 8-bit, not `GS_RGBA16F`.** OBS's own
  SDR render texture is 8-bit, so an 8-bit intermediate quantises on the same grid the
  direct path's write does, and round-to-8-bit is idempotent — the round trip
  reproduces the direct path's own bytes. A 16-bit float intermediate rounds onto a
  finer grid first and the 8-bit one second, flipping the last bit on a small
  percentage of pixels (`:1185-1192`).
- **The tile draw is one shared lambda** (`draw_tile`), used identically by both paths —
  same effect, technique, BT.709 conversion, cover-crop and `crop_uv`, same clamped
  border geometry. "A second copy specialised for that path is the one thing most
  likely to make a moving tile a different colour from a resting one, which is the
  acceptance gate for this whole feature. There is no second copy." (`:1877-1889`)
- **The fractional SIZE scale matters as much as the position.** Without it the leading
  edge moves smoothly while the trailing edge steps 2 px — "two edges of the same
  rectangle on different quantisation grids", which reads worse than the uniform
  stepping it replaced (`:1164-1173, 2131-2157`). The factor is within ~0.2% of 1.0.
- **The animator targets the SNAPPED rects, not `solved`** (`:1314-1336`). With
  `solved` as the target, every reflow ended with a single-frame jump from where the
  spring arrived to where the snap puts it — measured at 1920×1080 across 1..16 tiles
  reaching |dx| 2.67, |dy| 2.67, |dw| 1.56, |dh| 1.33 px, worst at 5..9 tiles, and
  **larger than the 1.947 px trailing-edge step the sub-pixel path exists to
  eliminate**, in the same place: the last frame of the transition.
- **Fade is a second quad, not a shader uniform** (`fade_tile_alpha`, `:1228-1251`):
  `gs_blend_function_separate(ZERO, ONE, ZERO, SRCALPHA)` with the `Solid` technique
  supplying `src.a` from `fill_color`'s alpha byte, so `dst.a := alpha * dst.a` and
  colour is untouched. Done this way because adding a global-alpha uniform would mean
  changing the `.effect` file, which a stale effect beside a new DLL would not have.

#### Output ordering contract [SHIPPED]

`TileAnimator::advance()` returns **held and exiting tiles first, live tiles from
`desired` last** (`src/zoom-tile-animator.h:32-39`). A single set change can hand a
departing participant's rect to a joiner, so a consumer **MUST** draw in the order
returned — that is what makes the live tile paint over a ghost at the same rect rather
than be hidden under it.

The render path resolves animator output to feeds via
`resolve_animated_for_desired(desired, animated)` (`:430-444`) — index-aligned with
`desired` **by construction**. The previous version re-read each feed's live slot state
for the lookup; `plan_feeds_locked()` can repoint a slot between the two reads (the
render path copies `shared_ptr`s and drops the mutex before drawing), so the lookup
either missed (that slot lost tile, border **and** glow for a frame) or collided with
another feed (a retired feed's grey placeholder painted over a live tile).

#### Roster-query gating [SHIPPED — perf-critical]

`src/zoom-supersource.cpp:1413-1451`. The roster is consulted **only** when
`anim.enabled && layout_disagrees_with_tracking(tracked, layout_ids)` — both halves
required. `ZoomEngineClient::roster()` deep-copies the whole participant list (one
`std::string` allocation per participant) while holding a mutex the engine reader
thread takes for **every** "frame" and "audio" message across every source. The earlier
`!tracked.empty()` gate bought a locked, allocating roster copy on all 60 frames a
second for the source's life.

---

## 7. MEMBERSHIP

`resolve_tile_assignments(previous, roster, params)` — `src/zoom-tile-fill.h:33-76`.

```cpp
enum class TileFillMode { Auto = 0, Manual = 1 };
struct TileFillParams {
    TileFillMode          mode      = Auto;
    std::size_t           max_tiles = 9;
    std::vector<uint32_t> excluded;   // Auto only; zeros are empty slots
    std::vector<uint32_t> manual;     // Manual only; zeros are empty slots
};
```

`kMaxTileSlots = 9` (3×3) — `src/zoom-supersource.cpp:92`. `kMaxExcludes = 3` — `:2252`.

### Auto mode [SHIPPED]

```
if (max_tiles == 0) return {}                       // :35

eligible(id) := id != 0
             && id not in params.excluded
             && exists p in roster with p.user_id == id AND p.has_video

// 1. incumbents first, in THEIR EXISTING ORDER
for id in previous:      if eligible(id) push_unique(id)
// 2. then newcomers, in ROSTER ORDER
for p in roster:         if eligible(p.user_id) push_unique(p.user_id)
// 3. truncate
if (out.size() > max_tiles) out.resize(max_tiles)
```

**"Roster order" is defined as the order of `ZoomEngineClient::roster()`'s
`std::vector<ParticipantInfo>`** — i.e. whatever order the Zoom engine reports
participants in. It is **only** consulted for *newcomers*.

**The stability rule is the point** (`src/zoom-tile-fill.h:24-29`): `previous` is what
is on the wall right now, and it is what makes the result stable. Participants still
eligible **keep their existing positions**; only new arrivals are appended.
"Rebuilding purely from roster order would let any SDK reordering move every face at
once, which on air is indistinguishable from a bug." Remaining participants never
*reorder* — they only **close up**
(`…participant-picker-design.md:34-40, 101-109`).

**Camera off ⇒ tile dropped and the wall reflows immediately** (Auto only), matching
Zoom gallery behaviour. Accepted cost: every camera toggle relayouts the wall on air.

### `max_tiles` interaction [SHIPPED]

- Slider `1..9`, default 9 (`src/zoom-supersource.cpp:3149-3151, 3113-3114`).
- Clamped `[1, kMaxTileSlots]` at update time (`:2599-2602`).
- **Auto-only.** In Manual mode it is force-overridden to `kMaxTileSlots`
  (`:2604-2615`):

```cpp
if (params.mode == TileFillMode::Manual) params.max_tiles = kMaxTileSlots;
```

Reason: the properties dialog hides `Max Tiles` in Manual mode, so a value the operator
lowered while in Auto must not go on quietly capping a Manual wall from behind a hidden
control. **This is applied in the OBS layer, not the pure resolver** — deliberately, so
the resolver stays one uniform rule and the mode-specific policy lives beside the UI
that owns the control.

- **Truncation drops the NEWEST**, since incumbents are emitted first
  (`…participant-picker-design.md:158` — "Auto honors `max_tiles`, truncating the
  newest").

### The never-show list [SHIPPED]

UI label `"Never show"` (`data/locale/en-US.ini:91`), keys `exclude_1..exclude_3`
(`src/zoom-supersource.cpp:2263-2266`). Three, because **OBS properties have no
multi-select control**: one covers the host account running the plugin, three covers
host + producer + tech; anything more elaborate is what Manual mode is for
(`…participant-picker-design.md:65-69`).

- **Auto-only** — `excluded` is never consulted in the Manual branch.
- Value `0` = `- none -`, ignored.
- Ids outside the 32-bit Zoom range are dropped, not wrapped
  (`src/zoom-supersource.cpp:2619-2623`).

### Manual mode [SHIPPED]

```cpp
if (params.mode == TileFillMode::Manual) {
    for (id : params.manual) push_unique(id);        // zeros and dupes dropped
    if (out.size() > params.max_tiles) out.resize(params.max_tiles);
    return out;                                      // ROSTER NEVER CONSULTED
}
```

**What manual assignment does when the assigned participant is absent:**

1. **The tile is kept.** "The roster is deliberately not consulted: an operator who
   cast a tile keeps it even while that participant's camera is off"
   (`src/zoom-tile-fill.h:49-51`). Manual mode is a **casting decision, not a liveness
   query**.
2. **The slot stays subscribed** and the engine keeps trying.
3. **The tile renders the neutral grey placeholder** — no frame, so
   `tile_take_snapshot()` fails and `tiles_draw_neutral()` runs, *with the operator's
   border at that tile's own rect* (`src/zoom-supersource.cpp:1910-1915`).
4. **A silent-slot resubscribe sweep** re-issues the subscription so a tile cast at
   somebody who has not joined yet fills in when they arrive
   (`src/zoom-tile-retry.h`, `src/zoom-supersource.cpp:613-709`):
   - `kTileSweepMinIntervalNs = 2 s` between whole sweeps.
   - Per-slot exponential backoff: `10 s, 20 s, 40 s, 80 s, then capped at 160 s`
     (`tile_retry_cooldown_ns(attempts) = 10s << min(attempts, 4)`).
   - `kTileRetryMaxAttempts = 6`, ≈5 minutes, then the slot **stops retrying and stays
     silent** rather than hammering the SDK.
   - The budget is tied to the slot's **assignment epoch**, so repointing hands it a
     fresh budget.
   - The test is per-epoch, not "never delivered anything" — a slot that showed a
     previous assignee has a non-zero generation forever.
5. **It must not be treated as a departure by the animator.** `classify_departures()`
   requires `ever_in_roster` evidence precisely so that repointing away from a
   never-joined Manual cast is classified as a **repoint** (instant cut, nothing on
   air), not a departure (`src/zoom-tile-animator.h:467-478`).

### Both modes [SHIPPED]

Zero ids dropped; duplicates collapse to their **first** occurrence
(`push_unique`, `src/zoom-tile-fill.h:42-46`). An empty result is legal and means an
empty wall — the wall still paints its background rather than disappearing
(`src/zoom-supersource.cpp:1271-1274`).

### Slot epoch / wrong-face prevention [SHIPPED — safety-critical]

`src/zoom-tile-slot.h`. A tile slot outlives the participants shown in it. Two ways the
wrong face reaches air, both closed lock-free:

1. A frame **in flight** for the previous assignee (engine dispatches asynchronously).
2. A frame **already stored** for the previous assignee (must stop being shown the
   instant the slot is repointed, or an incoming assignee with their camera off leaves
   the outgoing participant on air for the rest of the session).

```cpp
bool assign(uint32_t pid) {                 // :32-39
    if (m_participant_id.load(acquire) == pid) return false;   // no-op on same id
    m_participant_id.store(pid, release);
    m_epoch.fetch_add(1, acq_rel);          // id stored FIRST, THEN epoch bumped
    return true;
}
bool begin_frame(uint32_t event_pid, uint64_t &stamp) const {   // :81-85
    stamp = epoch();                        // EPOCH MUST BE SAMPLED FIRST
    return accepts_frame(event_pid);
}
static bool frame_is_current_at(uint64_t frame_epoch, uint64_t at_epoch) {
    return frame_epoch != 0 && frame_epoch == at_epoch;         // epochs start at 1
}
```

**The ordering is the guarantee** (`:62-80`). Sampling the id first leaves a window
where a repoint lands between the two loads: the id check passes against the OLD
participant while the frame is stamped with the NEW epoch, so a frame of the outgoing
participant is treated as current and stays on air until the incoming one sends a
frame — **forever, if their camera is off**.

`event_participant_id == 0` means the engine did not report one; **accept it** rather
than blanking the tile (`:52-57`).

---

## 8. BACKGROUND

### Compositing order [SHIPPED]

**background colour → background source → glow → tiles.**
(`data/effects/corevideo-tiles.effect:159`; `src/zoom-supersource.cpp:1639-1674`.)

**Colour** is drawn first, full canvas, through the `Solid` technique with a null
texture:

```cpp
fill_argb = picker_color_to_argb(ctx->bg_color);
gs_effect_set_color(param_color, fill_argb);
gs_technique_begin(solid);
if (gs_technique_begin_pass(solid, 0)) {
    gs_draw_sprite(nullptr, 0, canvas_w, canvas_h);
    gs_technique_end_pass(solid);
}
gs_technique_end(solid);
```

Default `0xFF808080`, grey in either byte order, matching the neutral fill the CPU
compositor used (`:3064-3066`). It replaces what used to be a fixed neutral fill drawn
through the I420 technique for CPU-path parity; the **per-tile placeholder is
untouched** and still uses the I420 path because it remains parity-critical
(`:1639-1646`).

**Colour byte order [SHIPPED, empirically determined]** — `src/zoom-supersource.cpp:762-780`:

```cpp
// obs_data colour int (picker and obs-websocket) is 0xAABBGGRR
// gs_effect_set_color() takes 0xAARRGGBB
static inline uint32_t picker_color_to_argb(uint32_t picker) {
    return (picker & 0xFF00FF00u) | ((picker & 0x000000FFu) << 16)
                                  | ((picker & 0x00FF0000u) >> 16);
}
```

Evidence recorded in-comment: setting `bg_color` to opaque red stored `0xFF0000FF`;
passed straight through, the gutter sampled `(0,0,255)` — **blue**. One helper serves
background, border and glow so they cannot drift.

**Background source** is drawn **outside** the technique, on purpose: it renders through
its own source's effect, so it cannot be drawn inside another technique's pass
(`:1669-1674`).

```cpp
// TilesBackground::render — src/zoom-tiles-background.cpp:124-148
gs_matrix_push();
gs_matrix_scale3f((float)canvas_w / sw, (float)canvas_h / sh, 1.0f);
obs_source_video_render(src);
gs_matrix_pop();
```

**Stretch to fill the canvas. Fit modes are explicitly out of scope**
(`src/zoom-tiles-background.cpp:140`, `…v2-design.md:208`). No aspect preservation.
No-op when `sw == 0 || sh == 0` or nothing is selected — which is why an unset
background leaves colour-only behaviour byte-for-byte unchanged.

### Recursion guard [SHIPPED]

`TilesBackground::set_source()` — `src/zoom-tiles-background.cpp:41-122`.

```cpp
// Register the parent/child link FIRST: this is the cycle check.
if (parent && !obs_source_add_active_child(parent, next)) {
    blog(LOG_WARNING, "Tiles background refused (would render itself): %s", name);
    obs_source_release(next);
    return TilesBackgroundResult::Refused;
}
```

- `obs_source_add_active_child()` **is** the cycle detector. Selecting the Tiles source
  itself, or a scene containing it, is an infinite render recursion — a stack overflow,
  i.e. OBS disappears mid-show (`…v2-design.md:135-138`).
- The check runs **before** the previous selection is retired, so a refused choice
  leaves a working background working (`:90-93`).
- On `Refused` the **setting is cleared**
  (`obs_data_set_string(settings, PROP_BG_SOURCE, "")`,
  `src/zoom-supersource.cpp:2779-2783`) — permanently refused, never retried, or the
  dropdown would go on showing a background that is not in effect and every update
  would re-log the refusal.
- `NotFound` is **deliberately not** treated that way — a name that has not resolved yet
  is the normal state during a scene load (a scene collection creates every source
  before it loads any of them), and clearing would erase the operator's setting on every
  restart. `tiles_source_load()` is the retry.
- The Tiles source itself **is** in the dropdown — the refusal has to hold for names
  arriving from a hand-edited scene file or obs-websocket too, not just the dropdown
  (`src/zoom-supersource.cpp:3174-3179`).
- `enum_active_sources` **must** be wired, or `obs_source_add_active_child` cannot see
  through a tiles source at all (`obs_source_enum_full_tree` returns early for a source
  with no `enum_active_sources`), so a cycle running through **two** tiles sources would
  be accepted and then crash the render thread (`src/zoom-tiles-background.h:75-79`).

### Fallback to colour [SHIPPED]

A **weak** reference is held, so selecting a background never keeps it alive after the
operator deletes it. `render()` returns early when the weak ref fails to resolve —
"deleted since selection; fall back to the colour"
(`src/zoom-tiles-background.cpp:134`). Only video-producing sources
(`OBS_SOURCE_VIDEO`) are listed (`src/zoom-supersource.cpp:3186-3192`).

The source is held **showing** (`obs_source_inc_showing`) for as long as it is
referenced, because a Media or Browser source in no active scene does not play and would
render a frozen first frame (`src/zoom-tiles-background.cpp:102-104`,
`…v2-design.md:139-144`).

**Documented, accepted race** (`src/zoom-tiles-background.h:27-39`): between
`add_active_child(next)` and the swap publishing `next` to `enum_active()`, and again
between the swap and `remove_active_child(prev)`, a child is a registered active child
that `enum_active()` does not report. A parent show/hide from another thread in that
window can leak or drop exactly one `show_ref`. Closing it would require holding
`m_mutex` across `add_active_child`, which walks the whole source tree and re-enters
`enum_active()` on any tiles source it meets — a self-deadlock. libobs's own scene items
carry the same window.

---

## 9. PER-PARTICIPANT AUDIO

### What it actually creates [SHIPPED]

**The wall itself carries no audio.** The property `Participant audio scene or group`
(`PROP_AUDIO_GROUP = "audio_group"`, `src/zoom-supersource.cpp:2249`) **names an
existing scene or group** in which the wall creates **one Zoom participant audio source
per tile**, so every person on the wall gets their own fader and their own ISO track.

- **The plugin does not create the scene or group.** "creating the group is the
  operator's act, and is also how they opt in" (`src/zoom-tiles-audio.cpp:194-198`).
  The name is resolved lazily, only when a `Create` actually needs it.
- Resolution uses `obs_group_or_scene_from_source()`, **not** `obs_group_from_source()`
  — the latter returns NULL for a scene, which would have rejected the safer option
  (`src/zoom-tiles-audio.cpp:239-245`).
- The property is **editable**, so a scene file naming a group that does not exist yet
  round-trips instead of silently resetting to "off" (`src/zoom-supersource.cpp:3196-3200`).
- Ships **empty** — the feature writes to the operator's scene collection and must not
  start doing that on upgrade for someone who never asked (`:2244-2248`).

### Ownership and the plan [SHIPPED]

`plan_tiles_audio(assignments, existing, roster, params)` — `src/zoom-tiles-audio-plan.h:72-176`.

Ownership is a **data marker**, `CV_TILES_AUDIO_OWNER_KEY = "cv_tiles_audio_owner"`,
holding the creating Tiles source's `obs_source_get_uuid()`
(`src/zoom-tiles-audio.h:18`). **Never by name** — the operator can rename anything, and
a name match would eventually let the plugin adopt (and mute) a source it did not create.
Sources without the marker are invisible to the planner and can never be chosen as
targets.

Actions: `Create`, `Adopt`, `Unmute`, `Mute`, `SetMixers`.

- **Owned by a different Tiles source ⇒ skipped entirely** — not created, muted, or
  retracked. Creating a second source would carry that voice twice and double them in
  the mix (`:113-119`).
- **Orphan** (marker present, owner gone) ⇒ **Adopt**, beating Create, because the
  operator's fader and any filters they added survive (`:134-142`).
- **Leaving the wall ⇒ Mute, never delete** (`:160-173`). Deleting would take their
  fader, filters and operator tuning with them, and in Auto mode the wall reflows
  constantly.
- Duplicate ids dropped explicitly even though the resolver already de-dupes — "a
  duplicate here would mean two slots claiming one voice" (`:104-108`).
- Display name: `"<display_name> (CoreVideo)"`, or `"Participant <id> (CoreVideo)"` when
  the participant is momentarily absent from the roster (`:88-96`).

### Track allocation [SHIPPED]

`tiles_audio_mixers_for_slot(slot)` — `src/zoom-tiles-audio-plan.h:65-70`:

```cpp
constexpr std::size_t kTilesAudioMaxTracks = 6;   // OBS provides six
constexpr uint32_t kProgram = 1u;                 // bit 0 = track 1 = program mix
if (slot + 1 >= kTilesAudioMaxTracks) return kProgram;
return kProgram | (1u << (slot + 1));
```

Track 1 is the program mix and **every** source joins it, giving a live fader for
everyone. Tracks 2..6 carry one ISO stem each — **five** of them — so the **sixth
participant onward is program-only**. `TilesAudioPlan::overflow` counts them so it can
be logged rather than silently swallowed.

### Documented multi-wall hazards [SHIPPED]

`warn_on_multiple_audio_walls()` — `src/zoom-supersource.cpp:2281-2336`; user-facing text
in `docs/CORE_PLUGIN_FUNCTIONALITY.md` Tiles §Per-participant audio.

**Multi-wall audio is not supported.** Audio for a participant is owned by exactly one
Tiles source ("one owner per participant"). Two consequences fall out the moment a second
wall names a group:

1. **Permanent mute of a visible participant.** Wall B owns P. P drops off B's wall, so B
   **mutes** P. P is still on wall A's wall — but A hits the "owned by another live Tiles
   source" guard and emits nothing at all. **P is muted while on screen and no reconcile
   will ever unmute them.**
2. **Colliding ISO stems.** Stem tracks are allocated per wall from track 2 up, so A's
   first tile and B's first tile both claim **track 2** — that ISO stem records two people
   mixed together.

Both are **silent** failures; a `LOG_WARNING` at least makes them findable. The warning
is latched (logged once per occurrence) and re-arms when the misconfiguration clears.
It reads each Tiles source's **settings**, not `ctx->audio_group`, because it runs inside
an `obs_enum_sources` callback holding `obs->data.sources_mutex` and taking a `ctx->mutex`
there would add a lock-order edge against a lock the graphics thread takes every frame.

### Other documented hazards [SHIPPED]

- **Add the scene/group to every scene**, or audio appears and disappears as you cut.
- **A nested scene is safer than a group** — a group can be dissolved by **Ungroup**,
  scattering the sources inside it (`data/locale/en-US.ini:127`,
  `src/zoom-tiles-audio.cpp:238-243`).
- **Clearing the field is an instruction, not an absence.** It means the operator turned
  the feature off, so it runs **one** reconcile with no assignments — muting everything
  this wall owns, creating nothing, touching nothing owned by anyone else. Tracked by
  `audio_off_pending` (`src/zoom-supersource.cpp:315-333, 2785-2800`;
  `src/zoom-tiles-audio.h:38-45`; `src/zoom-tiles-audio.cpp:374-419`). Reversible: naming
  a group again unmutes whoever is back on the wall. Previously an empty group name
  returned early, so "off" left every participant unmuted on tracks 1-6 in a group that is
  in every scene.
- **Process-wide serialisation.** `tiles_audio_reconcile()` runs scan → plan → apply as
  one atomic step under a single process-wide lock, on `OBS_TASK_UI`. Nothing else
  serialises two Tiles sources against each other; without it both could scan, both see
  nothing for a participant one is about to gain, and both plan a `Create` — the loser
  uniquifies into a genuine duplicate (`src/zoom-tiles-audio.h:20-36`).
- **Scene-collection load gate.** `s_collection_loading` starts **true** and blocks
  reconciles during collection change/cleanup. `obs_queue_task(OBS_TASK_UI, ...)` is *not*
  reliably deferred — it runs inline for a UI-thread caller, which a load always is — and
  creating into a dying group lands the new source in `ClearSceneData`'s orphan sweep,
  sets `clearingFailed`, and **closes OBS** (`src/zoom-supersource.cpp:113-134`).

**[GAP]** The exact source id / settings shape of the created per-participant audio
source was not read out (`obs_source_create` call at `src/zoom-tiles-audio.cpp:315` was
seen only in a grep). It is almost certainly the existing
`src/zoom-participant-audio-source.cpp` type, but I did not confirm the id string.

---

## 10. FAILURE MODES

### Effect fails to load — fatal vs. degrading [SHIPPED]

`src/zoom-tiles-effect-policy.h` + `src/zoom-tiles-effect.h`. This is a deliberate
two-tier policy, because **the DLL and `data/effects/corevideo-tiles.effect` install
independently** and it has already happened once that a new DLL shipped without syncing
`data/`. A stale effect still *compiles* — it is simply an older one — and every technique
or uniform the new DLL expects but the old file does not declare resolves to `nullptr`.

**FATAL** (`wall_drawable = false` ⇒ destroy the effect, `tiles_effect_load` returns
false, source draws nothing, loud log):

- the compiled effect itself
- technique `I420` and `image`, `tex_u`, `tex_v`
- `border_color`, `border_width`, `corner_radius`, `tile_size`, `crop_uv`
- technique `Solid` and `fill_color`

**DEGRADING** (`glow_drawable = false` ⇒ the wall draws, the glow pass is skipped for the
rest of the session):

- technique `Glow` and all eight `glow_*` uniforms

`glow_drawable` is gated on `wall_drawable` as well, so a caller reading it without
checking `wall_drawable` is never told the glow is available
(`src/zoom-tiles-effect-policy.h:169-173`).

Rationale, verbatim in intent: *"a stale effect file beside a new DLL must cost the
operator the feature it predates, never the whole wall. For a live broadcast that is the
difference between 'the glow is missing' and 'we are off air.'"*

**Why a missing glow uniform degrades the WHOLE glow rather than being individually
ignored** (`:41-62`): a null handle means the uniform is not declared in the effect file
**at all**, so there is no register for a partially-set pass to inherit from. Separately,
`gs_effect_set_*()` on a null parameter is null-safe but logs `LOG_ERROR` **per call** —
roughly **8 uniforms × 9 tiles × 60 fps ≈ 4,000 log lines per second** without the gate.
And a stale `Glow` technique was never written to consume the values a current build sets.

**Why the fatal set's parameters are not spared by the same argument** (`:64-75`): it is a
**stricter policy choice**, not a mirror image. These parameters govern the framing of the
operator's own video — crop, size, corner shape — not a decorative layer over it, and the
DLL has no way to tell "drew a plain square tile" from "drew the wrong crop of the source".
Blacking out the wall on a loud, logged error beats guessing.

**Known non-guarantee, stated in-source** (`src/zoom-tiles-effect-policy.h:78-82`,
`src/zoom-tiles-effect.h:45-51`): nothing compile-time ties `TilesEffectHandles` to
`TilesEffect`'s members. A handle added to one and left out of the other compiles cleanly,
defaults to `false`, and is caught only if someone notices the two structs have drifted.

### A tile has no frame [SHIPPED]

`draw_tile` falls through to `tiles_draw_neutral()` in **three** cases
(`src/zoom-supersource.cpp:1910-1915, 1954-1957`):

1. `!feed` (null feed pointer)
2. `!tile_take_snapshot(...)` — no valid pixels
3. `!tile_upload_frame(...)` — texture creation/upload failed
4. `crop_w == 0 || crop_h == 0` — degenerate source, nothing to map

`tile_take_snapshot()` returns false when (`:721-758`):
- `!feed->alive`
- no `has_frame` **and** the stored frame's epoch no longer matches the slot's ⇒
  `generation` forced to 0 ("Reassigned with nothing new yet: stop showing the old
  assignee")
- `generation == 0` (never had a frame)
- `width < 2 || height < 2`
- buffer shorter than `y_len + y_len/2` (not a full I420 frame)

**The neutral placeholder** (`src/zoom-supersource.cpp:833-882, 1059-1077`):
- Three **shared 1×1 `GS_R8` textures** holding `Y = 0x80`, `U = 0x80`, `V = 0x80`
  (`kNeutralY`/`kNeutralUV`, `:99-100`) — the exact bytes the CPU compositor wrote.
- Drawn through the **same `I420` technique**, so it is bit-identical to the old neutral
  fill *by construction*, rather than by an RGB constant that has to be trusted to round
  the same way.
- Uses the **same `TilePassParams`** as a video tile — so a no-frame slot gets the
  operator's border, radius and glow at **its own rect**, not the previous tile's.
- Drawn via `gs_draw_sprite()` (UVs already 0..1), so the default identity `crop_uv` of
  `(0,0,1,1)` is correct with no special case.
- **All three or none**: a half-built set is destroyed and rebuilt, because binding a
  null plane draws whatever the previous pass left there.
- If creation fails, `tiles_source_render()` returns immediately (`:1258`) — the whole
  wall does not draw — and logs **once**, not once per vsync.

### Other failure paths [SHIPPED]

| Failure | Behaviour | Cite |
|---|---|---|
| `canvas_w < 2 \|\| canvas_h < 2` | Render returns immediately | `:1262` |
| Empty feed list | **No early return** — the background still paints; `solve_tile_grid(0,…)` returns no rects so the tile loop does nothing. An empty wall must paint itself rather than disappear. | `:1271-1274` |
| `gs_technique_begin_pass` fails on `I420` | That tile's draw is **skipped** (`return` from `draw_tile`) — drawing after a failed begin_pass runs against whatever shader was loaded last, i.e. the wall drawn with some other source's effect. Logged once. | `:988-1018` |
| `gs_technique_begin_pass` fails on `Solid` (background) | Background not drawn; logged once. Presents as "the gutters went transparent" without the log. | `:1655-1666` |
| `gs_technique_begin_pass` fails on `Glow` | **`break`** out of the whole glow loop | `:1827-1832` |
| No default effect / no texrender / `gs_texrender_begin` fails | Tile **falls back to the snapped draw** — costs sub-pixel smoothness for that frame and nothing else. Per-tile failure is **latched** into `motion_composite_failed` so the glow pass agrees from the next frame. Logged once. | `:2162-2184, 1631-1637` |
| Tile rect `width < 2 \|\| height < 2` | Tile skipped (and its glow), "as the CPU path skips them" | `:1998, 1771` |
| Gutter/margin so large no tiles fit | Solver returns an empty layout ⇒ background alone. **Not an error.** | `…gallery-styling-design.md:99-100`, `src/zoom-tile-shape.h:74-81` |
| Custom aspect ≤ 0 or NaN | Falls back to 16:9 | `src/zoom-tile-shape.h:52-55` |
| Unknown preset id / unknown border shape from a newer scene file | Degrades to the option that existed first (16:9 / Square) | `src/zoom-tile-shape.h:57-59`, `src/zoom-supersource.cpp:2680-2686` |
| Background source deleted | Weak ref fails ⇒ colour only | `src/zoom-tiles-background.cpp:134` |
| Background source would recurse | Refused, logged, setting cleared | `src/zoom-tiles-background.cpp:94-100` |
| Audio group not found / renamed | `Create` actions skipped this call; `Unmute`/`Mute`/`SetMixers` still run (they never touch the group) | `src/zoom-tiles-audio.cpp:194-258` |
| Named source is neither scene nor group | New participants skipped this call, logged | `src/zoom-tiles-audio.cpp:246-252` |
| >5 participants with audio | Sixth onward is **program-only**; overflow counted and logged | `src/zoom-tiles-audio-plan.h:60-70, 121` |
| Silent slot after 6 attempts (~5 min) | Stops retrying, stays silent | `src/zoom-tile-retry.h:51-53, 81` |

All persistent-failure logs are **latched once, not per-vsync** — at 60 fps they would
bury every other line, but "a wall that silently will not draw is the worst symptom there
is" (`src/zoom-supersource.cpp:884-891`).

---

## 11. Colour conversion (not asked, but parity-critical)

`data/effects/corevideo-tiles.effect:69-84`. Included because a reimplementation that
gets this wrong will look "washed out or crushed" and blame the layout code.

```hlsl
float y = image.Sample(def_sampler, uv).r;
float u = tex_u.Sample(def_sampler, uv).r - 128.0/255.0;
float v = tex_v.Sample(def_sampler, uv).r - 128.0/255.0;

float r = y + 1.5748 * v;
float g = y - 0.1873 * u - 0.4681 * v;
float b = y + 1.8556 * u;
float3 rgb = saturate(float3(r, g, b));
```

- **BT.709, FULL range.** Full range means luma is **NOT** rescaled from 16-235.
  Applying a limited-range rescale "would wash every face out"
  (`data/effects/corevideo-tiles.effect:1-6`). This must match
  `set_yuv_frame_color_info()` in `src/zoom-source.cpp` (`VIDEO_CS_709` +
  `VIDEO_RANGE_FULL`, `full_range = true`).
- **Chroma offset is `128/255` (≈0.5019608), NOT a flat 0.5.** libobs builds its YUV
  matrix from `black_levels = {0, 128, 128}` and `format_conversion.effect` offsets chroma
  by the same `128/255`. Using 0.5 leaves a systematic ~1-LSB tint versus the CPU path.
- Sampler is `Linear` / `Clamp` / `Clamp`.
- The border is deliberately **downstream of and separate from** the colour maths: it only
  ever reads `rgb` and never alters how it was produced, "so the two cannot become
  entangled" (`:86-88`).

> ⚠ **Known open defect, from user memory not this repo:** the memory note
> *"CoreVideo color range defect"* (2026-08-11) records that CoreVideo hardcodes
> full-range BT.709 on what may be **limited-range** Zoom frames, measured washed out vs
> mimoLive. The shader above is the hardcode. Verify the SDK's actual range before
> copying this constant into a new compositor.

---

## 12. Uniform reference (complete)

`data/effects/corevideo-tiles.effect`.

### Shared

| Uniform | Type | Meaning |
|---|---|---|
| `ViewProj` | `float4x4` | Standard OBS view-projection |

### Technique `I420` (tiles + no-frame placeholder)

| Uniform | Type | Meaning |
|---|---|---|
| `image` | `texture2d` | Y plane, `GS_R8` |
| `tex_u` | `texture2d` | U plane, `GS_R8`, half size |
| `tex_v` | `texture2d` | V plane, `GS_R8`, half size |
| `border_color` | `float4` | Stroke colour; only `.rgb` used |
| `tile_size` | `float2` | Tile w/h in **canvas pixels** |
| `border_width` | `float` | Canvas pixels; 0 = no border |
| `corner_radius` | `float` | Canvas pixels; 0 = square |
| `crop_uv` | `float4` | `.xy` = origin, `.zw` = size, normalized texture coords |

### Technique `Solid` (background fill and the fade multiply)

| Uniform | Type | Meaning |
|---|---|---|
| `fill_color` | `float4` | Returned directly as the fragment |

### Technique `Glow`

| Uniform | Type | Meaning |
|---|---|---|
| `glow_color` | `float4` | Only `.rgb`; `.a` deliberately unused |
| `glow_quad_size` | `float2` | Expanded quad, canvas pixels |
| `glow_tile_center` | `float2` | Tile centre in **quad-local** pixels |
| `glow_tile_half` | `float2` | Half the **tile's** size, never the quad's |
| `glow_corner_radius` | `float` | Canvas pixels; the tile's **clamped** radius |
| `glow_size` | `float` | Falloff distance in canvas pixels; `> 0` |
| `glow_intensity` | `float` | 0..1, alpha at the tile's own edge |
| `glow_falloff` | `float` | `k` in `(1-t)^2(1+kt)`; 0..2, 0 = original |

**Per-tile upload rule [SHIPPED — a real trap]:** libobs uploads a pass's parameters
inside `gs_technique_begin_pass()` (`upload_parameters()`, `libobs/graphics/effect.c:209`)
and **does not re-upload them for later draws**. Therefore **every one of these must be
set BEFORE that tile's `begin_pass`, and every tile needs its own pass**. Rebinding inside
an already-open pass silently draws the previously-bound planes, or nothing. Both the
video path and the neutral placeholder funnel through `tiles_begin_pass()` for exactly
that reason (`data/effects/corevideo-tiles.effect:13-22`,
`src/zoom-supersource.cpp:973-991, 1021-1030`).

---

## 13. Settings reference (complete, with clamps)

| Key | Type | Default | Slider range | Hard clamp | Cite |
|---|---|---|---|---|---|
| `fill_mode` | int | `Auto` (0) | Auto/Manual | anything ≠ Manual → Auto | `:2593-2596` |
| `max_tiles` | int | 9 | 1..9 | `[1, 9]`; forced to 9 in Manual | `:2599-2615` |
| `exclude_1..3` | int (id) | 0 | roster list | `>0 && <=0xFFFFFFFF` else 0 | `:2619-2626` |
| `tile_1..9` | int (id) | 0 | roster list | same | `:2627-2628` |
| `canvas_width` | int | 1920 | — | `[16, 7680] & ~1` | `:103, 2652-2658` |
| `canvas_height` | int | 1080 | — | `[16, 4320] & ~1` | `:104, 2653-2659` |
| `bg_color` | colour | `0xFF808080` | — | none (byte-swapped) | `:3066` |
| `bg_source` | string | `""` | source list | cycle ⇒ cleared | `:3069, 2779-2783` |
| `tile_shape` | int | 0 (16:9) | 7 presets | unknown ⇒ 16:9 | `:3106-3107` |
| `tile_ratio` | double | 16/9 | 0.1..10.0 step 0.01 | `[0.1, 10]`; ≤0/NaN ⇒ 16:9 | `:3108, 2737-2745` |
| `gutter_pct` | double | `100/135` ≈ 0.7407 | 0..10 step 0.001 | `min(raw, 10)`; ≤0/NaN ⇒ 0 | `:3109, 2752-2758` |
| `margin_pct` | double | same | same | same | `:3110, 2759-2760` |
| `border_width` | int | 0 (off) | 0..64 | `[0, 64]`, then `clamp_border` | `:3075, 2667-2672` |
| `border_color` | colour | `0xFF000000` | — | byte-swapped | `:3076` |
| `border_shape` | int | Square (0) | Square/Rounded | ≠ Rounded ⇒ Square | `:3077-3078, 2680-2686` |
| `corner_radius` | int | 16 | 0..128 | `[0, 128]`, then `clamp_border`; folded to 0 when Square **at draw time** | `:3079, 2673-2676, 1683-1686` |
| `glow_size` | int | 0 (off) | 0..256 px | `[0, 256]`; **not** clamped vs gutter/margin | `:3085, 2695-2700` |
| `glow_color` | colour | `0xFFFFFFFF` | — | byte-swapped | `:3086` |
| `glow_intensity` | int | 100 | 0..100 % | `[0, 100]` | `:3087, 2701-2704` |
| `glow_softness` | int | 0 | 0..100 % | `[0, 100]` — **load-bearing** (`k ≤ 2`) | `:3092, 2708-2717` |
| `animate_layout` | bool | false | — | — | `:3098` |
| `animate_duration_ms` | int | 350 | 100..1000 step 10 | `[100, 1000]` | `:3099, 2726-2728` |
| `crop_left_1..9` | int | 0 | 0..45 % | `[0, 45]`, then `kMinCropRemainder` | `:3124, 2637-2646` |
| `crop_right_1..9` | int | 0 | 0..45 % | same | `:3125` |
| `audio_group` | string | `""` (off) | editable combo | clearing = "turn off", one reconcile | `:3128, 2762-2763, 2797-2799` |

**Every single feature defaults to OFF/identity**, and each one's comment states the same
guarantee: a scene saved before that control existed renders **byte-for-byte** as it did.
Preserve that structure — it is what made a long chain of features shippable into live
shows without regression.

---

## 14. Gaps — things I could NOT determine

Stated explicitly rather than inferred.

1. **[GAP] The per-participant audio source's OBS id and settings shape.** I saw
   `obs_source_create` at `src/zoom-tiles-audio.cpp:315` only through a grep. Almost
   certainly the type in `src/zoom-participant-audio-source.cpp`, but the id string and
   the settings keys it is created with were not read.
2. **[GAP] The exact `tile_feed_on_frame` ingest path** (`src/zoom-supersource.cpp:411-444`)
   — how `frame_epoch`/`generation` are stamped on arrival. The *rules* are fully
   documented in `src/zoom-tile-slot.h` and `src/zoom-tile-texture.h`; the call-site
   wiring was not read line-by-line.
3. **[GAP] No presentation clock / sync exists to specify.** The 2026-08-09 design's `L`,
   `frameAt(T)`, hold-last, starvation flag, and per-participant audio delay lines are
   **[DESIGN-ONLY]**. If the new compositor wants them, there is no shipped reference —
   only `src/tile-clock-probe.{h,cpp}` (Phase-0 timebase classifier:
   `kMinSamplesPerFeed = 30`, `kSharedTimebaseToleranceUs = 50000`,
   verdicts `Insufficient/Shared/PerFeed`) and the engine's TILECLOCK log lines.
   **I did not find a recorded verdict** for whether Zoom's `GetTimeStamp()` values share a
   timebase.
4. **[GAP] No name/label tile.** The 2026-08-09 design promised a "no-video name tile"
   (`:108-110, 147`); the shipped placeholder is a **flat neutral grey** with no text.
   Name-tile text is listed out of scope in both later specs.
5. **[GAP] Exit rendering.** Exits are computed by the animator but **never drawn**. What a
   held/exiting tile *should* draw is recorded as "still an open decision with the repo
   owner" (`src/zoom-supersource.cpp:1531-1538`). There is no shipped behaviour to match.
6. **[GAP] Actual glow visual parity vs. the reference image.** Unit tests cover the quad
   geometry only. The shader falloff and visual result are explicitly "not unit-testable
   and honest about it" (`…gallery-styling-design.md:102-107`).
7. **[GAP] Per-tile aspect, pagination, speaker-feature slot, top/bottom crop, drop
   shadows, inner glow, background fit modes, non-uniform gutters, drag-to-reorder.** All
   explicitly out of scope in the specs and absent from the code.
8. **[GAP] `docs/superpowers/plans/*` (phase-a 824 ln, phase-b 901 ln, animation 1213 ln)
   were not read end-to-end.** They are implementation plans; the shipped code is
   authoritative and was read in full for every claim above. If a plan contains a numeric
   value that contradicts something here, trust the code.

---

## 15. Reimplementation checklist — the things most likely to be got wrong

Ordered by how silently they fail.

1. **`resolve_spacing_px` divisor form.** Use `h / (100/pct)`, never `h * pct / 100`.
2. **Chroma offset `128/255`, not `0.5`.** ~1-LSB systematic tint otherwise.
3. **Slot-crop BEFORE cover-crop.** Reversed looks plausible on centred subjects.
4. **`crop_uv` from the TRUNCATED integers** handed to the subregion draw, and scale by
   those same integers. Otherwise borders misregister and a neutral sliver shows.
5. **Snap the whole grid once when settled; snap per-tile when not.** Never feed a
   mixed-size list to the whole-grid snapper.
6. **`even_floor_px(v) = uint32(v + 0.05) & ~1`** — the epsilon is the animator's rest
   epsilon, not slop.
7. **Separate alpha blend factors** (`ONE/INVSRCALPHA` for alpha) on every pass.
8. **Set every per-tile uniform before opening the pass; one pass per tile.**
9. **Glow centre is quad-local and absorbs canvas clipping** — not half the quad.
10. **Round the glow quad OUTWARD.**
11. **Sample the slot epoch BEFORE the participant id** when accepting a frame.
12. **Retarget springs unconditionally every frame.** No settle window, ever.
13. **Latch "has run an enabled frame" as its own flag.** Never infer state from an empty
    container — that shape caused four separate defects on this feature.
14. **`glow_falloff` clamped to `[0, 2]`.** Above 2 the curve rises and rings every tile.
15. **Composite intermediate must be 8-bit, blending off, straight alpha, with its own
    `ortho(0, w, 0, h, -100, 100)`.**
16. **One shared tile-draw routine for both the direct and composite paths.** No second
    copy.
