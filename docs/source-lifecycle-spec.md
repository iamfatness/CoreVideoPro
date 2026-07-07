# Source Lifecycle — Map, Unmap, Remove, Clean Up

Status: owner-requested 2026-07-06 ("We need a way to map and unmap sources, audio,
media. You can create but there is no clean up of anything. That is not manageable and
not good for a professional product"). Written the same evening, with a live case
study: **all 10 Show Input slots were found holding ghost assignments (8 parked
duplicates of one webcam), silently rejecting every new source** — hours of a
professional operator's confusion caused purely by missing lifecycle discipline.

## 1. The principle

Every entity an operator can CREATE gets, on the same surface where it appears:
**remove** (destroy it), **unassign** (detach it, keep it available), and — where a
device/session is involved — **disconnect** (stop the machinery). Every one of those
acts must tear down core-side resources and clean persisted state. Silent no-ops are
banned: an action that cannot proceed says why in CommandStatus.

## 2. Inventory (what exists today vs. what's missing)

| Entity | Create today | Missing lifecycle |
|---|---|---|
| Show Input slot assignment | pickers, auto-assign, bring-online | **Unassign** (slot back to empty), visible "assigned but source gone" state |
| Capture device connection (webcam/screen/SRT) | connect / bring-online | **Disconnect** (stop MediaCapture/WGC session/SRT listener, core teardown), remove virtual SRT sources |
| Zoom source rows (ISO + mix) | appear on join/subscribe | Rows persist after leave; no way to drop a participant's row/sends |
| Audio routing sends | matrix click | Bulk clear per source/bus; sends of DELETED sources linger in snapshots |
| Aux buses | Add Bus | **Delete/rename bus** (spec'd in audio-tab-redesign 4, never built) |
| Media assets | import to Media bin | **Remove from bin** (+ stop playback if live, drop slot references) |
| VST inserts | add to chain | Remove exists per-slot; whole-chain clear + orphaned-plugin cleanup missing |
| Scenes/layers | create/duplicate | Delete exists (S-series); layer references to dead sources linger |
| Persisted state | saved continuously | **No garbage collection**: ghost slots, stale device ids, dead sends accumulate forever |

## 3. Design

### 3.1 The lifecycle contract (per entity kind)
Core-side, every removable entity gets an explicit teardown command over the existing
JSON-RPC surface (`disconnect-capture-device`, `remove-media-asset`,
`clear-audio-sends {sourceId}`, `remove-aux-bus`), each answering with the
post-removal state so the shell renders truth, not hope. Teardown responsibilities:
stop sessions (WGC/MediaCapture/SRT), release SHM regions, drop mixer rows and their
DSP state, remove routing crosspoints, erase per-source telemetry.

### 3.2 Shell UX (uniform affordances)
- Every Show Input editor gets **Unassign** (slot → Unassigned kind, ids nulled).
- Every Sources row for a connected device gets **Take offline** (disconnect +
  auto-unassign its slots, with confirm when it is live on program).
- Media bin rows get **Remove** (confirm when referenced by a scene/slot; the confirm
  lists the references — no mystery).
- Matrix: per-source row context action **Clear all sends**; bus headers get
  **Rename/Delete** (delete blocked for the 5 fixed buses).
- Zoom rows: on participant leave, the row collapses to a "left the meeting" chip with
  **Remove row**; sends are kept 60s for rejoin, then swept.

### 3.3 Garbage collection (the ghost-slot cure)
On startup AND on every roster change, a sweep marks assignments whose source no
longer exists: slot chips render an "offline source" badge (not silent), and parked
(non-InShow) stale/duplicate assignments are reclaimed automatically (shipped 2026-07-06
as the assignment-time reclaim; the sweep generalizes it). Persisted state is
rewritten after sweep so ghosts never survive two sessions.

### 3.4 Laws
1. No silent no-ops (every rejected action reports why).
2. Remove is idempotent and safe mid-show (ramped audio, slate video, then teardown).
3. The snapshot is truth: UI state that the core no longer confirms gets a visible
   stale badge, never an invisible ghost.
4. Everything logged: create/assign/unassign/disconnect/remove each write one
   LaunchLog/media-core line (the 2026-07-06 debugging was blind for lack of these).

## 4. Slices (each lands as its own PR)

- **L1** — Show Input Unassign + stale/offline badges + startup ghost sweep.
- **L2** — Capture Take-offline (WGC/MediaCapture/SRT teardown core+shell).
- **L3** — Matrix: clear-sends per source; deleted sources drop their sends core-side.
- **L4** — Media bin Remove with reference confirm; playback stop on remove.
- **L5** — Zoom row lifecycle (leave chip, rejoin grace, sweep).
- **L6** — Aux bus rename/delete (completes audio-tab-redesign 4).
- **L7** — Persistence GC (rewrite-on-sweep; version the persisted schema).

L1+L2 are the operator-facing 80%: ~1 session together. L3–L7 follow demand.
