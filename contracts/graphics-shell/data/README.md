# Operator-style data inputs for the Mac graphics-shell vectors

Hand-written, like files an operator would load. Every EXPECTED value is computed by the real
Windows code (`native-shell/CoreVideoPro.MediaCore.Tests/GraphicsShellVectorTests.Data.cs`) and
committed in `../data.vectors.json`; never edit an expected value by hand.

| File | Rows | Columns (kind Windows infers) | Invariants the vectors rely on |
|---|---|---|---|
| `scores.csv` | 7 data rows, one blank line skipped | `team` text, `points` number, `qualified` boolean (blank cells allowed), `city` text | `team` is a unique key; `points` ties at 12 (`Zebras`, `Bees`: ordinal key order puts `Bees` first); `1e3`, ` 7 `, `3.50` are numbers; spaces around unquoted cells are trimmed; one quoted cell holds a comma, one holds `""`, one spans two lines; one non-ASCII team (`Élans`). Ends with a newline. |
| `crawl.csv` | 69 data rows (one blank line skipped) | `item` text | row 8 is a quoted empty cell (`""`), which a crawl filters out; more than 64 non-empty items, so the crawl limit applies after the filter. |
| `scores.json` | 5 rows | same four columns | `points` written `12`, `9`, `12`, `1e3`, `1.50` (raw text kept); `qualified` has `null`s; `Élans` and `Zürich` escapes; a `\"` escape. |
