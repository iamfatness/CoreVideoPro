#!/usr/bin/env bash
# Read-only: asks the running CoreVideo Pro (control API on 127.0.0.1:8011) what
# resolution each Zoom guest camera was subscribed at and what Zoom is actually
# delivering, plus whether the engine enabled HD video. Run during a meeting.
#   bash scripts/qa/check-zoom-receive-resolution.sh [port]
set -euo pipefail
PORT="${1:-8011}"
LOG="${HOME}/Library/Logs/CoreVideoPro-core.log"
TMP="$(mktemp)"; trap 'rm -f "$TMP"' EXIT
if ! curl -fsS -m 5 "http://127.0.0.1:${PORT}/snapshot" -o "$TMP"; then
  echo "FAIL  no control API on 127.0.0.1:${PORT} (is CoreVideo Pro running?)"; exit 1
fi
python3 -I - "$TMP" <<'PY'
import json, sys
snap = json.load(open(sys.argv[1])).get("snapshot", {})
churn = snap.get("zoomSubscriptionChurn") or {}
tiers = {0: "360p", 1: "720p", 2: "1080p"}
print(f"camera ceiling latched at join: {tiers.get(churn.get('cameraMaxResolution'), churn.get('cameraMaxResolution'))}")
rows = [s for s in churn.get("sources", []) if s.get("kind") == "participant-video"]
if not rows:
    print("WARN  no Zoom guest cameras subscribed (join a meeting with cameras on)"); sys.exit(0)
low = 0
for s in rows:
    w, h = s.get("receivedWidth") or 0, s.get("receivedHeight") or 0
    req = tiers.get(s.get("resolution"), s.get("resolution"))
    flag = "LOW " if h and h < 360 else "ok  "
    low += flag == "LOW "
    print(f"{flag} {s.get('sourceUuid')}  purpose={s.get('purpose')}  requested={req}  receiving={w}x{h}  frames={s.get('coreFramesIngested')}")
print(f"{low} of {len(rows)} guest cameras below 360p")
PY
echo "-- HD video evidence (core log)"
if grep -hE '"stage":"(enable_hd_video|create_setting_service_failed)"' "$LOG" 2>/dev/null | tail -1; then :; else
  echo "WARN  no enable_hd_video line in $LOG (engine predates the fix, or no sign-in since launch)"
fi
