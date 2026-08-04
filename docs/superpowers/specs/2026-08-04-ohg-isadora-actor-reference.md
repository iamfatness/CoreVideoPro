# Office Hours Global — Isadora JS Actor Reference

Extracted 2026-08-04 from `Experimental_OH-2.1_iz3.2.6_v0.78.1_AS_260315_092904.izz` (Isadora 3.2.6, patch v0.78).
Source blocks: `js_actors/` in this directory. This is the authoritative algorithm reference for the
CoreVideo Pro / OBS-plugin show-engine port.

---

## 0. Canonical data structures (referenced by nearly every actor)

**A. Raw Zoom participant record** (`Zoom_Cached_Data` output, keyed by zoomID string):
```json
{"16780288": {"userName":"Mickey Macachor","zoomID":16780288,"userRole":2,
  "onlineStatus":1,"videoStatus":1,"audioStatus":0,"handRaised":0}}
```
`userRole` is the ZoomOSC numeric role (1 = host, 2 = co-host, 3 = panelist/participant, 5 = attendee-ish; used only for display, the *editorial* role is separate).

**B. Zoom record after PIN + name/location extraction** (adds `hasPin`, `pin`, `displayName`, `location`).

**C. Mukana record** (external panelist registry, raw from REST, keyed by Firebase UID):
```json
{"0B6FTPaUEF…":{"displayName":"J.J. Mc Kenna","host":true,"loc":"Santa Venetia, CA, US",
  "pin":1383,"online":true,"ts_off":1773578962002,"role":"panelist","uid":"…"}}
```

**D. Parsed Mukana DB** (keyed by 4-digit PIN):
```json
{"1383":{"displayName":"J.J. Mc Kenna","location":"Santa Venetia, CA, US",
  "pin":1383,"role":"panelist","online":true}}
```
`role` ∈ `panelist | host | reader | aslpanelist | aslinterpreter`.

**E. Aggregated Panelist DB** (`DataBase_Aggregator` output, keyed by zoomID):
```json
{"16780288":{"name":"Roy Meyers | dd02 | Forest Hill, MD | US","displayName":"Roy Meyers",
  "location":"dd02","pin":" ","hasPin":false,"hasMukana":false,"role":"panelist",
  "zoomID":16780288,"videoStatus":1,"onlineStatus":1}}
```

**F. Live VideoPanelist DB / "Panelists w VideoOn"** (keyed by zISO slot 1..N, i.e. video pin index; `route` == that slot):
```json
{"1":{"name":"…","displayName":"Ray Franklin","location":"","pin":" ","hasPin":false,
  "hasMukana":false,"role":"panelist","zoomID":16793600,"videoStatus":1,"onlineStatus":1,"route":1},
 "9":{"displayName":"EMPTY","zoomID":0,"route":9}}
```
Empty slots are literally `{"displayName":"EMPTY","zoomID":0,"route":N}`. Some older versions key by `"<index>-<zoomID>"` (e.g. `"3-50379776"`) or `"<index>-"`; `key.split("-")[0]` is the position.

**G. Gallery DB** (16 multiview cells):
```json
{"1":{"gRoute":1,"gInput":1,"update":1,"zoomID":16778240,"displayName":"Ray Franklin","pin":" "},
 "11":{"gRoute":11,"gInput":0,"update":1,"zoomID":0}}
```
`gRoute` = multiview/router destination (1-based), `gInput` = source route (= a panelist's zISO `route`; 0 = blank), `update` = dirty flag.

**H. ZoomISO instances DB** (from `infraestructure-*.js`):
```json
{"JohnZoomISO-1":{"address":"10.5.9.91","port":9090,"videoPins":8,"audioPins":8,
  "zak":"","manJoin":false,"videoRole":true,"oscRole":true}}
```

**I. Pin-assignment record** (`VideoPanel_DB` output):
```json
{"zInstance":"JohnZoomISO-1","address":"10.5.9.91","port":9090,"update":1,
 "zoomID":0,"videoPin":1,"audioPin":1}
```

---

# 1. Ingest / Zoom event layer

## `Zoom_Cached_Data`
**Purpose:** Stateful in-memory cache of every Zoom participant, driven by discrete ZoomOSC event pins.

**Inputs (19):** `User Name`, `ZoomID`, `User Role`, `Online Status`, `Video Status`, `Audio Status`, `Hand Raised`, `Sanitize Name String` (regex, default `/[\n\r]+/g`), `Append` (trigger: add/replace record), `Finish Update`, `User Name Update`, `ZoomID Update`, `VideoOn`, `VideoOff`, `Online Status`, `Offline Status`, `UserName Changed`, `isLive`, `Dump All`.
**Outputs:** `Zoom Data` (JSON of `participantsLast` snapshot), `Data Ready` (bool), `DEBUG`, `Participants Data` (JSON of live `participants`), `# Participants`, `# ParticipantsLast`.

**Algorithm:** Two module-level dicts `participants` (working) and `participantsLast` (published snapshot).
- `Append==1` → write full record at `participants[zoomID]`, name sanitized via `userName.replace(sanNameString,'')`; then early-return the snapshot.
- `Dump All==1` → clear both.
- `VideoOn/VideoOff==1` → mutate `participants[zoomIDUpdate].videoStatus = 1/0`, set `finishUpdate`.
- `Online==1` → create a fresh record with `userRole:5, onlineStatus:1, videoStatus:0`.
- `Offline==1` → overwrite the record with `userName:"OFFLINE USER", userRole:5, onlineStatus:0`. (Elsewhere the string is `"USER OFFLINE"` — inconsistent; both appear in sample data.)
- `UserNameChanged==1` → update `.userName`.
- On `finishUpdate` → `participantsLast = participants; dataReady = true`.
There is a commented-out "isLive filter" that would drop OFFLINE slots during pre-show.

**Protocol:** driven by `/zoomosc/user/{list,online,offline,videoOn,videoOff,userNameChanged,activeSpeaker,isSpeaking,mute,unMute,handLowered}` and `/zoomosc/me/{list,videoOn,videoOff,userNameChanged}`, plus `/zoomosc/pong`, `/zoom/list`.

## `ZoomData_Pin_Extractor`
**Purpose:** Scan every participant's `userName` for a 4-digit Mukana PIN.
**In:** `Zoom JSON Data`, `RegEx String` (default `\b(\d{4})\b`). **Out:** same JSON with `hasPin: bool` and `pin: <string>|" "` added to each record.
**Algorithm:** `new RegExp(regexString,'g')`, `userName.match(regex)`; takes the **first** match, trimmed. No match → `hasPin=false, pin=" "`.

## `3ZoomData_Display_Name_Location_fallback_Extractor`
**Purpose:** Fallback split of the Zoom display name into `displayName` + `location` when there's no Mukana record.
**In:** `Zoom JSON Data`, `Separator String` (default `[/|]`, applied as `new RegExp(sep,'ig')`). **Out:** JSON with `displayName`/`location` filled.
**Rules** on `userName.split(regexSep)`:
- `result.length > 2 && hasPin` → `[displayName, location] = result` (first two segments; e.g. `Roy Meyers | dd02 | Forest Hill, MD | US` → `Roy Meyers` / `dd02`).
- `result.length == 2 && !hasPin` → `[displayName, location] = result`.
- otherwise → `displayName = result[0]`, `location = " "`.
Both trimmed; missing location becomes `" "`.

---

# 2. Mukana (external panelist registry) layer

## `Mukana_Data_Parser`
**In:** `Mukana JSON Data` (raw UID-keyed), update flag, `Purge Data`. **Out:** `Parsed Mukana`.
**Algorithm:** Re-key by PIN; project `{displayName, location: element.loc, pin, role, online}`; skip records without `pin`. Accumulates into a persistent `mukanaDB` (never removes stale entries unless purged). `Purge Data==1` → `{}`.

## `Mukana_Data_Merger`
**In:** `Parsed DB`, `Cached DB`, `Trigger Merge DB`, `Trigger Dump DB`. **Out:** `Mukana Merged` JSON.
**Algorithm:** persistent `mukanaMergedDB`; if cachedDB non-empty adopt it wholesale, then `Object.assign(merged, parsed)` (parsed wins). `trigDump` clears. Note the merge trigger is commented out — it merges on every call.

## `Mukana_DB_expander` (×4 duplicates)
**In:** `Mukana JSON Data`. **Out:** four newline-joined strings: `Display Name`, `Location`, `Pin`, `Role` — parallel arrays for downstream index-based lookup.

## Mukana REST health gate (`Javascript__89/90`)
**In:** raw Mukana response text. **Out:** `Update` (0/1). Returns 1 only if the body does **not** contain `"status"` — the REST endpoint returns `{"status":200,…,"detail":"This page is only available between 1300 and 2000 UTC…"}` outside show hours, and that must not be parsed as panelist data.

## Override DB / role assignment engine (`Javascript__93`)
**Inputs (19):** `Mukana DB`, `Override DB`, `Pin`, `Display Name`, `Location`, `Role`, `Trig Ins/Upd Override DB`, `Pin Over`, `Display Name Over`, `Location Over`, `Role Over`, triggers, `Override Del`, `Set Host`, `Set Reader`, `Set Panelist`, `Set ALS Panelist`, `Set ALS Interpreter`, `Dump Override DB`.
**Output:** the override DB JSON (`{pin:{pin,displayName,location,role}}`).
**Algorithm:** persistent `overrideDB`, merged with any incoming override DB. Insert/update writes the `*Over` fields at key `pinOver`. `Set Host` / `Set Reader` enforce **exclusivity**:
1. `panelistsRole(mukanaDB, "host")` → every Mukana-declared host not already in overrideDB is copied in and demoted to `"panelist"`.
2. Every current override with that role: if it's also a Mukana host it is demoted to `"panelist"`; otherwise its override row is deleted.
3. Then `overrideDB[pinOver]` is (re)written and set to `"host"` (resp. `"reader"`).
`Set Panelist` / `Set ALS Panelist` / `Set ALS Interpreter` just write the row with role `panelist` / `aslpanelist` / `aslinterpreter`. `Dump` clears.

## Override list encoder (`Javascript__92`) + `_Override_DB_Panelists_Selector_Decoder_v3`
Encoder: newline list of `pin+displayName+location+role` (separator `+`). Decoder: `line.split(sep)` by 1-based selected index → `Pin`, `Display name`, `Location`, `Role`.

---

# 3. Aggregation

## `DataBase_Aggregator`
**Purpose:** Join Zoom participants with the Mukana registry into the master panelist DB.
**In:** `Zoom JSON Data`, `Mukana JSON Data` (PIN-keyed), `Update Data`, `Purge Data`, `Cleaned Data`. **Out:** `Panelists JSON Data`, `Data dirty`.
**Algorithm:** For every Zoom record: `mukanaPanelistData = panelist.hasPin ? newMukanaData[panelist.pin] : false`.
- **Mukana hit** → `displayName/location/pin/role` from Mukana, `hasMukana=true`.
- **Miss** → use the fallback `displayName/location/pin` already stamped on the Zoom record by the two extractors, `role` defaults to `"panelist"`, `hasMukana=false`.
Emitted record = shape **E** (`name` is always the raw `userName`). `Purge`/`Update` reset the accumulator; `Cleaned Data==1` clears the dirty flag. (Shipped code has bugs — `.lenght` typo, `dirtyData` vs `dataDirty` mismatch — reimplement cleanly.)

---

# 4. Video-pin / ZoomISO routing

## `ZoomISO_Instances_Filter`
**In:** `Zoom bots Label`, `Zoom bots Data` (structure **H**), `filter by` (`videoRole` or `oscRole`). **Out:** filtered map, `ZoomISO count`, `OSC Bot` (last matching bot).
**Algorithm:** keep bots where `bot[role] === true`. Separates ISO video bots from the single ZoomOSC control bot.

## `VideoPanel_DB`
**Purpose:** Allocate panelists to concrete ZoomISO video/audio pins.
**In:** `ZoomISO Instances Data`, `Panelists W VideoOn` (structure **F**). **Out 1:** `{instanceName:{pinIndex: pinRecord}}`; **Out 2:** `Total Video Pins`; **Out 3:** flat `{globalPin: pinRecord}`.
**Algorithm:** iterate instances in key order; for each instance emit pins `1..videoPins`; a running `totalVideoPins` counter indexes into `panelistsDB[totalVideoPins+1]` to pull the `zoomID` (0 if empty). Global pin *N* maps to an instance and local `videoPin`/`audioPin` within it. Record shape **I**, `update:1` marks it for transmit. Each record carries the instance's `address`/`port` for per-bot OSC addressing.

## `ZoomISO_Input_List`
Mutable ordered name list. **In:** `Name to Add`, `Name to Emplace`, `Location to Emplace` (1-based, decremented), `Emplace`, `Add`, `Reset`. **Out:** `"<n>   <name>"` per line + raw names.

## `Panelists_VideoStatusOn_filter` / `_Panelists_VideoStatusOn_Pin_filter`
Base: keeps `videoStatus && onlineStatus`. Pin variant additionally requires `hasPin`. Outputs filtered JSON + count.

## `Panelists_Append_to_the_End`
Stable ordered assignment: persistent key array; unseen zoomID appended, `route = keys.length`. `Dump Data` resets. "First-come, keeps its slot forever."

## `Panelists_Append_Remove_Replace_v4` ← **the core live-slot manager**
**Inputs (15):** `Live VideoPanelist DB`, `Updated VideoPanelists` (master DB), `zoomID to Add`, `Index to Add`, `Add Panelist`, `zoomID to remove/replace`, `Index to remove/replace`, `Remove Panelist`, `Replace Panelist`, `Dump Data`, `Available Video Pins`, `Graphics Bots at End`, `Force update`, `Update Live Panelists Data`, `Backup Panelist DB`.
**Outputs:** `Panelists w VideoOn` (structure **F**), count, backup DB JSON.

**Helpers:**
- `getNextEmpty(panelists)` — forward scan for first slot whose `displayName` contains `"EMPTY"`.
- `getNextEmptyFromEnd(panelists)` — same scanning reversed keys.
- `resetPanelistRole(p)` — for every other slot with the same `role` and different `route`, force `role = "panelist"`. **Single-host / single-reader invariant.**
- `getPanelistFromIndex(panelists, index)` — `some()` over keys comparing `key.split("-")[0] == index`.

**Behaviour:**
- Startup: if empty and a backup exists, restore from backup.
- `Dump Data` → clear everything.
- **Add:** look up master DB by zoomID. **Graphics-bot rule:** if `hasPin && pin >= 9000`, `pinInc = pin - 9000`, take `getNextEmptyFromEnd()` then subtract `pinInc` from key and index, `route = index + 1`. Otherwise `getNextEmpty()`, `route = index`. Host/reader → `resetPanelistRole` first.
- **Remove:** replace slot in-place (key preserved) with `{displayName:"EMPTY", zoomID:0, route: livePosIndex - 1}`.
- **Replace:** new panelist inherits the old slot's route, role exclusivity applied, written over old key.
- **Bulk rebuild:** pre-fill `1..availableVideoPins` with EMPTY, re-add every panelist from master DB with same PIN≥9000 / next-empty logic; pad with `"<n>-"` keys.
- **Update Live Panelists Data:** for every occupied slot, re-pull master record by zoomID, re-apply role exclusivity, refresh fields in place (slot stays put).

## `VideoPanelistDB_Expander`
Fan-out of live DB into five parallel newline-joined strings in key order: names, locations, PINs, routes, roles. Index-aligned lingua franca for SuperSource/MixEffect/graphics actors.

## `_LiveVideoPanelistsSelectorList_Genereator_v3`
**In:** `VideoPanelist DB`, `Available Video Pins`. **Out (3 encodings):**
1. `index;displayName;pin-or-####;zoomID`
2. `index-displayName`
3. `index. displayName` (first 16 — FS-HDR/multiview list)

## Full list encoder (`Javascript__8`)
`panelistItem(index, p, sep)` emits:
```
index ; displayName ; (videoStatus?"V":"#") ; (hasPin?pin:"####") ; (hasMukana?"M":"#") + roleSuffix ; zRoute|# ; zoomID
```
role suffixes: `.H` host, `.R` reader, `.AP` aslpanelist, `.AI` aslinterpreter, `.P` panelist.
Sentinels: index `0` = `CLEAR` (`0;CLEAR;#;0000;#;0;0`), index `25` = `SCREEN` (`25;SCREEN;V;0000;#;25;1`). EMPTY rows pad to 40. Merge step pushes each live entry's `route` onto `panelist.zRoute` (array — a person can hold several ISO pins).

## Gallery-merge encoder (`Javascript__9`)
Same against Gallery DB: pushes `gRoute` arrays, emits `index;displayName;V|#;pin|####;M|#;gRoute|#;zoomID`.

## Selector decoders
- `_Live_Video_Panelists_Selector_Decoder_v2`: field `[3]` = zoomID from `n;NAME;PIN;ZOOMID`.
- `Video_Panelists_Selector_Decoder`: split `;`, field `[5]`.
- `_Javascript_-_List_Selector_Decoder`: `list[index-1]` verbatim.
- `_Javascript_-_PanelistSelected_decoder`: split (default `-`) → `[Route=r[5], Index=r[0], ZoomID=r[6], full]`.

---

# 5. Speaker-driven routing (FILO / score-based)

## `FILO_Speaker_Router`
FILO assignment of active speakers to a fixed pool of ~20 ISO channels.
**In:** `Current Speaker` (name), `Original Users`, `Original User Routes`, `Refresh Users`, `Refresh Routes`.
**Out:** `Name to Get` (`""` = no-op), `Channel to Route` (`-1` = no-op), `User Order`, `Channel Assignments` text.
**Algorithm** on persistent parallel arrays:
- Pool < 20: push speaker, `chan = max(channels)+1`.
- Already present: splice out, re-push to end (recency refresh), emit nothing.
- Pool full: `shift()` oldest, newcomer inherits that channel.
Channels 1-based internally, printed 0-based.

## `Smart_Gallery_Brain`
Recency-ordered gallery by "speaking score".
**In:** `Video Users` (gallery order), `Active Speaker`, `Serialized Zoom UserNames`, `Serialized Routes`, `Reset Scores`, `Add Score`. **Out:** route numbers, most-recent-first.
**Algorithm:** persistent `speakingScores[20]`. On `Add Score`: increment every score by 1; set speaker's to 0. Bucket-sort ascending score (stable) → names → routes.

## `MV16_Router`
Keeps the most-recent 16 speakers in the 16 visible multiview cells by swapping.
**Algorithm:** persistent users/routes/scores (scores seeded `[0..19]`). Speaker at index `k`: if score ≠ 0, all scores +1, `scores[k]=0`. If `routes[k] >= 16` (off-screen): find `maxLoc` = stalest visible index, swap `routes[maxLoc] ↔ routes[k]`, `scores[maxLoc]=0`, `scores[k]=max` (inverted-looking but shipped behaviour).
**Out:** `"<k> <route>"` per line (0..15), scores debug, search routes.

## `MV16_Routing_Generator`
Gallery DB (**G**) → newline `"<gRoute-1> <gInput-1 or 0>"` — 0-based dest/src pairs for the BMD router. `gInput==0` → source `0` (black).

## `Javascript_-_List_Transposer`
Gallery DB + `Module` (grid dim) → transposed DB (`indexList[j*module+i]`) + non-zero zoomID count. Row-major ↔ column-major for hardware numbering.

---

# 6. Gallery management

## `Gallery_Remove_Replace_v2` / `v3`
**Inputs (14):** `Live VideoPanelist DB`, `Updated GalleryDB`, `Cleaned GalleryDB`, selection (`zISO Route`, `Gallery Route`, `zoomID`), actions (`Remove`, `Replace`, `Insert` (stub)), bulk (`Reset From zISO`, `Empty Gallery`).
**Outputs (v3):** `Gallery DB`, count, sep, `SmartGallery` DB, its count.
**Algorithm:** persistent `galleryDB` (+ `smartGalleryDB` in v3), `gallerySize = 16`.
- `emptyGallery()` → all cells `{gRoute:i+1, gInput:0, update:1, zoomID:0}`.
- `defaultGallery()` → identity `{gRoute:i+1, gInput:i+1}`.
- `Cleaned GalleryDB` → merge acknowledged state back.
- `Reset From zISO` → `defaultGallery()` then pack all non-EMPTY live panelists into cells 1..16 (`gInput = panelist.route`).
- `Replace` → `livePanelistsDB[zRoute]` into `galleryDB[galleryRoute]`.
- `Remove` → v2: `gInput = galleryRoute` (identity), `zoomID = 0`. v3: additionally `smartGalleryDB[galleryRoute].gInput = 0`.
- `galleryCount()` = cells with `zoomID != 0`.

---

# 7. Search / lookup actors

## Active Speaker Search family (v5 → v6)
- **v5**: In: `LiveVideoPanelists`, `ZoomID`. Out: name/location/pin/route/role/videoOn. Linear match; bug: no-match leaves last-inspected entry.
- **v4-ish "Full Data Set"**: adds fallback DB + route, override route (lower-third uses that slot's occupant).
- **v6 (current)**: adds `Skip Roles` (e.g. `"aslinterpreter"`) + persistent `lastPanelist`. No match + fallback → fallback DB; matched but role in skip list → keep `lastPanelist` (ASL interpreter never steals lower third / auto-cut). Outputs speaker 7-tuple + lower-third 7-tuple.

## `_Participant_on_Position_Search_Full_Data_Set_v1`
Match both `zoomID` and position (`key.split("-")[0] == position`) — disambiguates the same person on two ISO pins.

## `Search_Host_Reader_v2`
**In:** live DB, `Role` (`host`/`reader`), `Manual assignment`, `Panelist Pin`, `Update`. **Out:** name/location/pin/route.
Auto mode: match `panelist.role == role` (last wins). Manual: match `pin == panelist.pin && pin != 0`.

## `Search_Multiple_Host_Reader`
Conflict detector across master DB / live DB / Mukana DB: counts + `"name: pin"` arrays for host and reader per source → operator warning when duplicated.

## `Display_Name-Location_Host`
`indexOf("host")` / `indexOf("reader")` in serialized roles → same index into names/locations. `null`/`"?"` → `''`.

## `Name_Builder_v2`
Join non-empty args with spaces; also returns `result.split("-")[1]` as ZoomID.

---

# 8. SuperSource / ATEM layer

## SuperSourceBrain (`Javascript__13`) — hands-queue → SuperSource box layout
**In:** `HandsAPI` (3 lines: upcoming CSV | current PIN | previous CSV; `NONE` sentinel), `SuccessAPI`, `Max Boxes` (4), `Host Pin`, `Reader Pin`, `SuperSource State`.
**Out:** `Super Source Name` (e.g. `"1234"`, `"34"`, `"NOSOURCE"`), `Updated` (on change), `Search Pins` (newline), counts.
- `stripPin(pin)` removes host (and reader when `superSourceState <= 2`) PIN from queue text.
- `parseData()`: current PIN unshifted to front, upcoming pushed, previous counted as invisible prefix. Dupes suppressed.
- `updateStatus()`: `boxesArray` = "1..maxBoxes" repeated; window `boxesString.substring(invisible, invisible + maxBoxes)` = box-name string, one digit per active box.

## `SuperSource_Search_v3`
**In:** live DB, `Search PINs`, `ID for Blank`, `Super Source Name`, `Run Alg`. **Out:** 8 routes (boxes 1–8).
For each char `c` at index `j`: `box = parseInt(c)-1`; find live panelist with `pin == searchPins[j]`; `outRoutes[box] = panelist.route` else `idForBlank`.

## `Display_Name_In_The_Box` / `Location_In_The_Box`
Same shape resolving names/locations: `indexSerPin = arrayOfPINs.indexOf(pin)` → `arrayOfOrderedNames[indexSerPin]`, guarded by `i < superSourceName.length`. 8 outputs. (Identical code; wiring determines names vs locations.)

## `MixEffect_Info_v15` — ATEM full-state parser + editorial tally
**Inputs (17):** ATEM state JSON, `Host PIN`, `Reader PIN`, `SuperSource State`, `Hand PINs`, `Serial Routes/PINs/Names`, `MultiView Format`, 4× `Remote Route/PIN` pairs.
**Outputs (88):** ME1–ME4 PREV/PROG/FTB, USK fill/key/onAir, 4 DSK blocks (`Number, ON AIR, Key Source, Fill Source, Masked?, Is Auto Transition?, Rate, PreMultiply?`), `Cascade?`, SSRC1/2 preset + boxes 1–4, `ATEM Name`, then PREV Mode/PINs/Routes/Names and PROG same.

**ATEM state schema:** `{name, me[].{preview,program,ftb,usk[].{fillSource,keySource,onAir}}, dsk[].{index,onAir,keySource,fillSource,masked,isAutoTransitioning,rate,preMultiplied}, superSource.{cascade, ssrc[].{currentPreset, boxes[].source}}}`.

**Tally derivation** (PREV from `me1prev`, PROG from `me1prog`):
- `1..20` → `"Direct Input"`; that route's person is on air.
- `10020` → `"Active Speaker"`; person = ME2 **program**.
- `21`/`10030` → `"Smart Gallery"`; every route `1..multiViewCount(format)` live.
- `6000` → SuperSource; mode from state number:
  `1 SuperSource HR Q`, `2 SuperSource HR`, `3 SuperSource H Q`, `4 SuperSource H`, `5 Banter HG Q`, `6 Banter HG`, `7 Teatime HTG Q`, `8 Teatime HTG`, `9 Panel Checks`.
  - `"panel check"` → only ME2 program counts.
  - otherwise: host PIN always; reader PIN when `superSourceState <= 2`.
  - `"banter"|"teatime"|"remote"` → add SSRC1 boxes 1–2; `"quad"` → boxes 1–4.
  - `cascade == true` → also SSRC2 boxes 1–4 (hands row).
- `25 Screen`, `34 Countdown`, `35 Background`, `3020 Start Card`, `3030 End Card`, default `Unknown`.
- Remote override: if a `Remote Route N` is in the route list, force-push `Remote PIN N`.
- `prevPINs = pins.filter(v => v!=="" && v!=="?" && v!=="0").join(",")` (empty → `"0"`).

**Source-ID constants:** `1..20` inputs, `21` multiview/gallery, `25` screen, `34` countdown, `35` background, `1000/2001` black/bars, `3010/3011` DVE fill/key, `3020` start card, `3030/3031` end card, `3040/3041` DSK4 graphic, `6000/6001` SuperSource, `10020` active-speaker aux, `10030` smart-gallery aux.
`multiViewCount(layout)`: `"3"`→9, `"4"`→16, `"2"`→4, `"s"`→1, default 16.

## `onProgram_-_onPreview`
Software PGM/PVW bus model. `preview` → set preview; `cutDirect` → `preview = program; program = new`; `cut` → swap.

## `SPX_SSRC_Fields_Builder`
Per-box names/locations → SPX field payload `[{field:"f0",value:layout}, {field:"f1",…}]`.
**Layouts:** `1`-series host-only, `2`-series host+reader; A/B/C/D = 1/2/3/4 guest boxes:
| Layout | f1/f2 | f3/f4 | f5/f6 | f7/f8 | f9/f10 | f11/f12 |
|---|---|---|---|---|---|---|
| A1 | host | box1 | — | — | — | — |
| B1 | host | box1 | box2 | — | — | — |
| C1 | host | box1 | box2 | box3 | — | — |
| D1 | host | box1 | box2 | box3 | box4 | — |
| A2 | host | reader | box1 | — | — | — |
| B2 | host | reader | box1 | box2 | — | — |
| C2 | host | reader | box1 | box2 | box3 | — |
| D2 | host | reader | box1 | box2 | box3 | box4 |
Each pair = (name, location).

---

# 9. SPX graphics (REST) layer

Base URL: `"http://" + ip + ":" + port` (production `http://10.5.9.144:5656`, backup `http://10.5.9.71:5656`).

**Rundown / item control:**
| URL |
|---|
| `{spx}/api/v1/item/play/{id}`, `/continue/{id}`, `/stop/{id}` |
| `{spx}/api/v1/item/{play,continue,stop}` (focused item) |
| `{spx}/api/v1/rundown/load?file={file}/{rundown}` |
| `{spx}/api/v1/rundown/{focusFirst,focusNext,focusPrevious,stopAllLayers}` |

**Template functions** — prefix `{spx}/api/v1/invokeTemplateFunction?playserver=OVERLAY&playchannel=1&playlayer=10&webplayout=10&function={fn}&params={payload}`:
| function | params |
|---|---|
| `animateHeadlineIn` | `s1 + "|" + s2` (name\|location, not URI-encoded) |
| `changeHeadline` | `encodeURIComponent(s1) + "|" + encodeURIComponent(s2)` |
| `animateHeadlineOut` | — |
| `animateQuestionIn` | `encodeURIComponent(JSON.stringify({name,loc,cc,q,v,ts,tag}))` (q: newlines → spaces) |
| `animateQuestionOut` | — or `s1|s2` |
| `runAnimationIN` | — |

**Hands/questions API record:**
```json
{"q":{"key":"-Mms66PcbK_9cAj550wX","n":"Douglas Carmichael","q":"…","tag":"Zoom ISO",
      "ts":1635176445667,"v":-1},
 "hands":{"prev":[],"curr":[],"next":[]}}
```

Char-code fingerprint (`Σ charCodeAt`) used as cheap change-detector for text (`Javascript__35/68/70/91`).

---

# 10. Configuration loaders

`_Load_Data_From_File_V4` uses Isadora `include(filename)`, file defines `var data = {…}`; outputs flat `[key, value, …]` alternating array (objects stringified).

**`infraestructure-oh.js` shape:**
```js
var data = {
  zoombots:  {"JohnZoomISO-1":{"address":"10.5.9.91","port":9090,"videoPins":8,"audioPins":8,
                               "zak":"","manJoin":false,"videoRole":true,"oscRole":true}, /* …, */
              "JohnZoomOSC":{"address":"10.5.9.169","port":9090,"videoRole":false,"oscRole:":false}},
  hands:     {"address":"10.5.9.90","port":9090},
  panelists: {/*…*/}, universe:{"address":"127.0.0.1","port":3050,"listSeparator":";"},
  mixeffect: {"address":"10.5.9.62","port":49990,"feedbackPort":8080},
  spx1:      {"address":"10.5.9.144","port":5656},
  spx2:      {"address":"10.5.9.71","port":5656},
  isadora:   {"address":"10.5.9.205","port":1234},
  heartbeat: {"address":"10.5.9.169","port":1234,"interval":5,"running":0},
  tally:     {"address":"https://oh.tally.video"}
};
```
Note the typo `"oscRole:"` (trailing colon) on the ZoomOSC bot — real gotcha.

**Mukana REST URLs:**
```
https://hoka.pxclabs.com/phpsdk/php-panel-rest.php?event=officehours&req=panelists
…&req=question
…&req=hands
```

---

# 11. Synthesis — end-to-end data flow

```
ZoomOSC (UDP 9090)                      Mukana REST (hoka.pxclabs.com)
  /zoomosc/user/{list,online,offline,       ?req=panelists / question / hands
   videoOn,videoOff,userNameChanged,             │
   activeSpeaker,isSpeaking,handLowered}         │  health gate (reject "status" error body)
        │                                        ▼
        ▼                                  Mukana_Data_Parser  (UID-keyed → PIN-keyed)
  Zoom_Cached_Data                               ▼
        ▼                                  Mukana_Data_Merger  (persistent merged DB)
  ZoomData_Pin_Extractor  (+hasPin,+pin)         ▼
        ▼                                  Override DB (host/reader exclusivity, ASL roles)
  fallback name/location extractor               │
        └────────────────┬───────────────────────┘
                         ▼
              DataBase_Aggregator      ← master Panelist DB, keyed by zoomID
                         ▼
        Panelists_VideoStatusOn(_Pin)_filter
                         ▼
   Panelists_Append_Remove_Replace_v4  ← operator add/remove/replace, PIN≥9000 bots-at-end,
                         │                 role exclusivity, backup/restore
                         ▼
              LIVE VIDEO PANELIST DB  (slot 1..N == route == ZoomISO video pin)
        ┌────────────────┼────────────────────────────┬──────────────────────┐
        ▼                ▼                            ▼                      ▼
 VideoPanel_DB   VideoPanelistDB_Expander    Gallery_Remove_Replace_v3   Host/Reader + Active
 (pin records)   (5 parallel lists)          (16-cell Gallery DB)        Speaker searches
     ▼                     │                        ▼                          ▼
 OSC → each ZoomISO        │                 MV16_Routing_Generator      SuperSource_Search /
 bot (its own IP:9090)     │                        ▼                    Name/Location In Box
                           │                 ATEM/BMD multiview                ▼
                           │                        ▲                    SPX_SSRC_Fields_Builder
                           │            Smart_Gallery_Brain / MV16_Router      ▼
                           ▼             / FILO_Speaker_Router            SPX REST
                  MixEffect_Info_v15  ←── ATEM full-state JSON (MixEffect)
                   → PREV/PROG mode, PINs, routes, names (tally)
```

## Distinct subsystems

1. **Zoom event ingest & cache** — ZoomOSC OSC → zoomID-keyed participant map with PIN/name/location enrichment.
2. **Mukana registry sync** — REST poll, error-body gate, PIN re-keying, persistent merge, operator Override DB with host/reader exclusivity + ASL roles.
3. **Panelist DB aggregation** — join (2) into (1) on PIN → single source of truth for identity + editorial role.
4. **ZoomISO pin allocation** — instance filtering, slot management, global slot → `{instance, address, port, videoPin, audioPin, zoomID}`. PIN≥9000 "graphics bots pinned to the end".
5. **Recency / speaker-driven routing** — FILO eviction, score bucket-sort, score-based swap into visible 16.
6. **Gallery / multiview** — 16-cell Gallery DB, remove/replace/reset-from-zISO, gallery vs smart-gallery, transposition, `"<dest> <src>"` command generation.
7. **SuperSource / hands queue** — hands API → box-layout name (`"1234"`); resolution into routes/names/locations per box.
8. **ATEM state & tally** — full switcher state → which humans are on preview/program (PIN/route/name lists), incl. remote-contributor PIN forcing.
9. **SPX graphics** — rundown/item control + six template functions; `f0..f12` field payload keyed to layouts A1–D1/A2–D2.
10. **Serialization / UI glue** — parallel newline lists, semicolon list encodings with flags and sentinels (`CLEAR`=0, `SCREEN`=25), matching decoders.
11. **Configuration** — `include()`-based infra/meeting files defining every device address/port and REST URL.

### Reimplementation gotchas
- `Object.keys(x).some(cb)` used as "find"; on no-match the loop variable retains the **last** element — several actors depend on/suffer from this.
- Live-DB key formats vary: `"1"`, `"1-"`, `"1-50379776"`. Position is always `key.split("-")[0]`.
- Shipped typos: `.lenght`, `udpate`, `dirtyData`/`dataDirty`, `"oscRole:"`, `smatGalleryDB`.
- Routes 1-based in JSON, emitted 0-based to router/ATEM (`gRoute-1`, `gInput-1`, `iso_channels[k]-1`).
- `pin` is `" "` (single space) when absent, `"####"` in list encodings — never empty string.
