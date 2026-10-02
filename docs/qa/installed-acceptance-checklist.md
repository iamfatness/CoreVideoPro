# Installed-beta acceptance checklist

One session on an installed beta, to accept or reopen the merged fixes that only the
owner can judge. This is BACKLOG Now 1 (2026-10-02). It is a checklist, not a work queue:
each row's status lives on its issue.

**How to record.** On each issue, comment the beta id and one of `PASS`, `FAIL` (say what
you saw) or `MISSING_EVIDENCE` (the condition could not be produced). A `PASS` closes the
issue; anything else leaves it open.

**Before starting.** Install the beta. Join the test meeting with the full link including
`pwd` (the bare meeting id fails). Engine on.

## Part 1: automated, no operator input

Run with the app in the meeting. Each reads only the control API on `127.0.0.1:8011`.

| Issue | Command | Passes when |
|---|---|---|
| baseline | `python scripts/qa/live-check-sources-audio.py 60` | Exit 0: wall guests have video, the app muted no guest, meters live, snapshot fresh. |
| #732, #725 | `python scripts/qa/core-restart-drill.py --takes 3 --record` | Exit 0: core rejoins, roster is the new core's, a new recording runs in a new folder, the earlier file plays. |
| #739 | `GET /snapshot`, read `captureDevices` | No `decklink-1` or `aja-io-1`. |
| #740 | `GET /snapshot` before pressing Record | No `recording` node; the transport shows no recording. |

## Part 2: operator alone

| Issue | Do this | Passes when |
|---|---|---|
| #757 | Overlays page: turn on a bug and a lower third with Zoom capture on. Wait 30 s. | Both stay on Program and stay "On" in the list. |
| #759 | Start an RTMP stream. | The status reads Starting, then Live once media flows; it never says Live while connecting. |
| #740 | Record for 20 s, then Stop. | The file exists at the path shown and plays. |
| #724 | While streaming, Take a clip that has never been cued to Program. | No visible hitch on Program or the stream. |
| #473 | Add a ProRes clip as a source in the installed build. | It plays; no placeholder tile. |
| #568 | Start an AV1 stream (refused), switch to H.265, start again without restarting the app. | The H.265 stream starts; no stale AV1 failure text. |
| #569 | Stream H.265 to a receiver. | The receiver plays picture and sound. |
| #758 | Connect a camera that first falls back to the bridge and then connects natively (or reconnect one). | The tile shows live video, not a frozen frame. |
| #760 | Add an SRT or RTMP ingest source and send to it. | The source reads connected, with frame age (and RTT for SRT). |
| #735 | On a machine with no hardware encoder, stream to RTMP and to SRT. | Both deliver picture and sound. Otherwise `MISSING_EVIDENCE`. |

## Part 3: needs real guests

| Issue | Do this | Passes when |
|---|---|---|
| #657, #608 | A guest mutes and unmutes in Zoom; another guest leaves. | The shell strip and the audio follow the mute within the refresh budget, with nothing on Preview; the remaining guests' audio does not drop. |
| #725 | With a camera-off participant in the show, Take between scenes. | The multiview second row does not reflow. |
| #668 | Host plus three guests; run interview, solo and share-priority. | Each guest holds one slot, the host is not followed, Set & Forget holds. |
| #674 | Open the multiview input inspector; a guest turns video off; arm ISO; Engine off and on. | Format, ISO and picture update without clicking the row. |
| #582 | Let the active speaker change several times. | No brightness flash on the cut. |
| #581 | Compare the Zoom window with Program for the same guest. | Program is not visibly slower. |

Not on this list because code remains: #728 (the low-memory warning is not yet shown to the
operator), #703 (needs a fresh YouTube playback measurement), #762 and #741 (Now 2).
