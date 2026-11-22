# Owner acceptance: macOS control and audio, Phase 2 (RC5, RC6, A3, A4, A6)

Plan: `docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md`. Outcomes:
`docs/superpowers/plans/2026-11-21-mac-control-audio-phase2-outcomes.md`.
Everything below needs the **installed, signed** build from this branch, launched normally. The
implementing agent ran none of it: the owner's app was live on 127.0.0.1:8011 and no test-launched
app was started. Record results inline.

## 1. RC5: Companion on the same Mac (owner check 4 of the remote-control spec)

1. Build the module once: `cd companion-module-corevideopro && npm install && npm run build`.
2. Read-only check against the running app (changes nothing on air):
   `node scripts/companion-mac-live-check.mjs --module companion-module-corevideopro --base http://127.0.0.1:8011`
   — expect `companion live check: PASS`. Result:
3. In Companion, add a CoreVideo Pro connection with Host **`127.0.0.1`** (not `localhost`, not the
   Mac's LAN address: see the module README's macOS section), port 8011.
4. On a physical Stream Deck, drop the seven presets. Expect: FTB, Take, Record, Stream, Engine and
   Lower-third act and light; **Auto-assign does nothing** (not on the Mac yet, by design). Note any
   button that does not light. Result:

## 2. RC6: Volt rundown

1. Stage a clip: copy an `.mp4` to `~/Library/Application Support/CoreVideoPro/media/Volt Rundown/show-open.mp4`.
2. `curl -s -H 'Content-Type: application/json' -d '{"action":"media.rundown.load","args":["show-open"]}' http://127.0.0.1:8011/invoke`
   → `{"ok":true,"data":{"sourceId":"media:media-…"}}`. Result:
3. Pick a scene that is not on Program and whose canvas has one visible layer (or note a layer id).
   `… -d '{"action":"media.rundown.preview","args":["show-open","<sceneId>",""]}'` → `previewSceneId`;
   the clip shows in Preview. Result:
4. `… -d '{"action":"media.rundown.take","args":["show-open","<sceneId>",""]}'` → the clip goes to
   Program with audio. Repeating it answers "The staged clip is already on Program…". Result:
5. A recording made across step 4 shows the clip's first frame within a frame or two of the Take. Result:

## 3. A3: mastering presets and A/B

1. Audio > SETUP: Starting point > Podcast. The rack reads target -16, glue 0.45, low cut 70 Hz. Result:
2. Tweak glue, press B (B recalls Neutral), press A (your tweak is back). Result:
3. Save a preset "Sunday", quit the app, relaunch: the rack, the A/B slot and "Sunday" are all back,
   and the first thing you hear is the saved rack (not bypass). Result:
4. Toggle Master protection off; a hot source now clips on the master meter; on again, it does not. Result:

## 4. A4: meters (measured only)

1. Play a steady tone (or a talker) for a minute with the rack in. INTEGRATED climbs and settles near
   the target marker; TRUE PEAK stays below the ceiling marker (turns red only above it). Result:
2. The console's master rail shows two meters that move together for a centred source, and a number
   that matches the footer's LUFS. Result:
3. In a real meeting, a guest who mutes themselves in Zoom shows **ZOOM MUTED** under their strip, their
   strip's MUTE stays off, and the meter goes dim. Unmuting in Zoom clears it at once. Result:
4. Engage COMP on a loud channel: the COMP cell's GR readout moves. Result:

## 5. A6: buses and LISTEN

1. Audio > ROUTING shows 15 bus columns (scroll sideways), routed counts and a chip strip. Result:
2. "+ Bus" adds BUS 1; route a guest to it; toggle → MASTER on the chip: that guest is heard on master
   through the bus (and twice as loud if also routed straight to master). Result:
3. Monitor on, headphones: LISTEN on ISO 1 with one guest routed to it plays only that guest. LISTEN off
   returns to the monitor mix. Result:
4. Remove BUS 1: its column, sends and LISTEN go; nothing else changes. Result:
