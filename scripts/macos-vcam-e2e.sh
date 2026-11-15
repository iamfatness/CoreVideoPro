#!/bin/bash
# Local end-to-end check of the CoreVideo Pro macOS virtual camera (roadmap
# M9.2, Task 7; docs/virtual-camera-macos-spec.md). Runs on the owner's Mac
# with the signed app installed in /Applications and the camera extension
# approved. CI cannot run it: CI runners cannot activate system extensions.
#
#   scripts/macos-vcam-e2e.sh [--app] [--restart-extension]
#
# Exit 0 only if every step that runs passes. The first failing step prints
# "E2E FAIL (step N): ..." and the script exits 1. Every run writes
# dist/vcam-e2e/<timestamp>/ with the probe JSONs, the feeder logs, the
# extension's unified log (subsystem us.iamfatness.corevideopro.camera) and
# every [virtualcam] line.
#
# Steps:
#   1. /Applications/CoreVideo Pro.app exists; systemextensionsctl lists the
#      camera extension "activated enabled" at the app's CFBundleVersion.
#   2. No producer: probe 3 s at 30 fps -> fps >= 28, slateFrames == frames.
#   3. Build corevideo-vcam-feed, sign it (Developer ID, hardened runtime,
#      identifier us.iamfatness.corevideopro.vcam-probe), check it satisfies
#      the extension's producer requirement, feed red for 6 s; probe 3 s from
#      t=1 s -> programFrames >= 0.9*frames, centerY 82 +-3.
#   4. Probe 2 s starting 1 s after the feeder exits -> slateFrames == frames
#      (spec §4 law 2: no frozen frame).
#   5. --restart-extension (sudo): kill the extension process while feeding ->
#      the feeder is live again within 3 s and the probe sees program again.
#   6. --app: launch the app, enable the camera through the control API
#      (transport.virtualcam.set, Task 11), probe 3 s -> program, snapshot
#      framesPublished advancing; quit the app -> slate within 1 s. Skipped,
#      loudly, while the control API has no transport.virtualcam.set.
#   7. leaks --atExit over a 300 s feed (VCAM_E2E_LEAK_SECONDS) -> no
#      CMSampleBuffer / CVPixelBuffer leaks.
#
# Environment:
#   COREVIDEO_NATIVE_BUILD   native build dir configured with
#                            -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON (native/build-metal)
#   COREVIDEO_SIGN_IDENTITY  (Developer ID Application: John Wallace (42394L3353))
#   VCAM_E2E_LEAK_SECONDS    step 7 feed length (300)
#   JUDGE_LEAKS_LOG          self-test: judge this saved leaks log as step 7
#                            would, and do nothing else
#
# Camera permission: the probe captures from Terminal (or whichever app runs
# this script). macOS asks once; if access was denied the probe says how to
# grant it, and the step fails.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

APP="/Applications/CoreVideo Pro.app"
EXT_ID="us.iamfatness.corevideopro.shell.camera"
EXT_EXECUTABLE="CoreVideoProCamera"
FEED_IDENTIFIER="us.iamfatness.corevideopro.vcam-probe"
LOG_SUBSYSTEM="us.iamfatness.corevideopro.camera"
IDENTITY="${COREVIDEO_SIGN_IDENTITY:-Developer ID Application: John Wallace (42394L3353)}"
NATIVE_BUILD="${COREVIDEO_NATIVE_BUILD:-native/build-metal}"
LEAK_SECONDS="${VCAM_E2E_LEAK_SECONDS:-300}"
CONTROL="http://127.0.0.1:8011"
PROBE="$ROOT/mac-shell/.build/release/VcamProbe"

RUN_APP=0
RUN_RESTART=0
for arg in "$@"; do
    case "$arg" in
        --app) RUN_APP=1 ;;
        --restart-extension) RUN_RESTART=1 ;;
        *) echo "usage: $0 [--app] [--restart-extension]" >&2; exit 2 ;;
    esac
done

OUT="$ROOT/dist/vcam-e2e/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
LOG_START="$(date '+%Y-%m-%d %H:%M:%S')"
FEED="$OUT/corevideo-vcam-feed"
FEED_PID=""

now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time*1000'; }

collect_logs() {
    log show --start "$LOG_START" --style compact \
        --predicate "subsystem == \"$LOG_SUBSYSTEM\"" >"$OUT/extension.log" 2>&1 || true
    cat "$OUT"/feed-*.log 2>/dev/null | grep '\[virtualcam\]' >"$OUT/virtualcam.log" || true
}

cleanup() {
    if [ -n "$FEED_PID" ] && kill -0 "$FEED_PID" 2>/dev/null; then kill "$FEED_PID" 2>/dev/null; fi
    collect_logs
    echo "artifacts: $OUT"
}
trap cleanup EXIT

fail() { echo "E2E FAIL (step $1): $2"; exit 1; }
pass() { echo "E2E ok   (step $1): $2"; }
skip() { echo "E2E SKIP (step $1): $2"; }

# json <file> <key> -> the value (plutil reads JSON).
json() { plutil -extract "$2" raw -o - "$1" 2>/dev/null; }

# probe <name> <seconds> [extra args...] -> writes $OUT/<name>.json; non-zero on probe failure.
probe() {
    local name="$1" seconds="$2"; shift 2
    "$PROBE" capture --seconds "$seconds" --json --frames-csv "$OUT/$name.csv" "$@" \
        >"$OUT/$name.json" 2>"$OUT/$name.err"
    local code=$?
    [ -s "$OUT/$name.err" ] && sed 's/^/    probe: /' "$OUT/$name.err"
    return $code
}

# start_feed <name> <seconds>: runs the signed feeder in the background.
start_feed() {
    "$FEED" --seconds "$2" --pattern red >"$OUT/feed-$1.log" 2>&1 &
    FEED_PID=$!
}

wait_feed() {
    local code=0
    wait "$FEED_PID" || code=$?
    FEED_PID=""
    return $code
}

# Step 7's verdict on a `leaks --atExit` log. A pass needs positive evidence:
# leaks' own "Process N: K leaks for …" summary (absent when leaks could not
# inspect the process — then nothing was checked) and the feeder's final JSON
# reporting "status":"live". Only then does "no CMSampleBuffer/CVPixelBuffer in
# the report" mean anything. The feeder's own output never names those types.
judge_leaks_log() {
    local log="$1" summary
    grep -q '"status":"live"' "$log" \
        || fail 7 "the feeder did not end live under leaks (no \"status\":\"live\" in its final JSON; see $log)"
    summary="$(grep -E '^Process [0-9]+: [0-9]+ leaks? for ' "$log" | head -1)"
    [ -n "$summary" ] \
        || fail 7 "no leaks summary line (\"Process N: K leaks for …\"): leaks did not inspect the feeder (see $log)"
    grep -qE 'CMSampleBuffer|CVPixelBuffer|CVBuffer' "$log" \
        && fail 7 "leaks reported CMSampleBuffer/CVPixelBuffer leaks (see $log)"
    pass 7 "no CMSampleBuffer/CVPixelBuffer leaks over $LEAK_SECONDS s ($summary)"
}

# Self-test of the step 7 verdict against a saved or fixture log; runs nothing
# else (no app, no extension, no feed).
if [ -n "${JUDGE_LEAKS_LOG:-}" ]; then
    trap 'rmdir "$OUT" 2>/dev/null' EXIT
    judge_leaks_log "$JUDGE_LEAKS_LOG"
    exit 0
fi

# ── 1. installed and activated ──────────────────────────────────────────────
[ -d "$APP" ] || fail 1 "$APP is not installed"
APP_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Contents/Info.plist" 2>/dev/null)"
[ -n "$APP_VERSION" ] || fail 1 "cannot read CFBundleVersion of $APP"
systemextensionsctl list >"$OUT/systemextensions.txt" 2>&1
EXT_LINE="$(grep -F "$EXT_ID" "$OUT/systemextensions.txt" | grep -F '[activated enabled]' | head -1)"
[ -n "$EXT_LINE" ] || fail 1 "systemextensionsctl does not list $EXT_ID as [activated enabled] (see $OUT/systemextensions.txt)"
# systemextensionsctl prints "(<one version>/<other version>)"; either half may be CFBundleVersion.
echo "$EXT_LINE" | grep -Eq "[(/]${APP_VERSION//./\\.}[/)]" \
    || fail 1 "active extension version does not match the app's CFBundleVersion $APP_VERSION: $EXT_LINE"
pass 1 "extension activated enabled at CFBundleVersion $APP_VERSION"

# ── 2. no producer: slate at the declared rate ──────────────────────────────
if [ ! -x "$PROBE" ]; then
    (cd mac-shell && swift build -c release --product VcamProbe) >"$OUT/build-probe.log" 2>&1
fi
[ -x "$PROBE" ] || fail 2 "missing probe binary $PROBE (cd mac-shell && swift build -c release --product VcamProbe; log: $OUT/build-probe.log)"
probe step2 3 --fps 30 || fail 2 "probe failed (see $OUT/step2.err)"
FRAMES="$(json "$OUT/step2.json" frames)"; FPS="$(json "$OUT/step2.json" fps)"
SLATE="$(json "$OUT/step2.json" slateFrames)"
awk -v f="$FPS" 'BEGIN { exit !(f >= 28) }' || fail 2 "fps $FPS < 28 ($(cat "$OUT/step2.json"))"
[ "$FRAMES" -gt 0 ] && [ "$SLATE" = "$FRAMES" ] || fail 2 "slateFrames $SLATE != frames $FRAMES with no producer"
pass 2 "slate only: $FRAMES frames, $FPS fps"

# ── 3. the signed feeder puts program on the camera ─────────────────────────
grep -q 'COREVIDEO_WITH_MAC_VIRTUALCAM:BOOL=ON' "$NATIVE_BUILD/CMakeCache.txt" 2>/dev/null \
    || fail 3 "$NATIVE_BUILD is not configured with -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON"
cmake --build "$NATIVE_BUILD" --target corevideo-vcam-feed >"$OUT/build-feed.log" 2>&1 \
    || fail 3 "building corevideo-vcam-feed failed (see $OUT/build-feed.log)"
cp "$NATIVE_BUILD/corevideo-vcam-feed" "$FEED"
# get-task-allow lets step 7's `leaks` inspect the hardened process. This copy is
# a local test tool, never notarized or shipped.
cat >"$OUT/feed.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
    <key>com.apple.security.get-task-allow</key><true/>
</dict></plist>
PLIST
codesign --force --options runtime --timestamp=none --sign "$IDENTITY" \
    --identifier "$FEED_IDENTIFIER" --entitlements "$OUT/feed.entitlements" "$FEED" \
    >"$OUT/sign-feed.log" 2>&1 || fail 3 "signing the feeder failed (see $OUT/sign-feed.log)"
# The extension's own admission rule (spec §4 law 6), read from the source of truth.
REQUIREMENT="$(sed -n 's/.*producerRequirement = #"\(.*\)"#.*/\1/p' mac-shell/Sources/CameraExtensionKit/CameraConstants.swift)"
[ -n "$REQUIREMENT" ] || fail 3 "cannot read CameraConstants.producerRequirement"
codesign --verify -R="$REQUIREMENT" "$FEED" >>"$OUT/sign-feed.log" 2>&1 \
    || fail 3 "the signed feeder does not satisfy the producer requirement: $REQUIREMENT"
start_feed step3 6
sleep 1
probe step3 3 --fps 30 || fail 3 "probe failed while feeding (see $OUT/step3.err)"
wait_feed || fail 3 "feeder did not end live (see $OUT/feed-step3.log)"
FRAMES="$(json "$OUT/step3.json" frames)"; PROGRAM="$(json "$OUT/step3.json" programFrames)"
CENTER="$(json "$OUT/step3.json" centerY)"
awk -v p="$PROGRAM" -v f="$FRAMES" 'BEGIN { exit !(f > 0 && p >= 0.9 * f) }' \
    || fail 3 "programFrames $PROGRAM < 0.9 x frames $FRAMES"
[ "$CENTER" -ge 79 ] && [ "$CENTER" -le 85 ] || fail 3 "centerY $CENTER is not 82 +-3 (red, BT.601)"
pass 3 "program: $PROGRAM of $FRAMES frames, centerY $CENTER"

# ── 4. producer gone: slate, never a frozen frame ───────────────────────────
sleep 1
probe step4 2 --fps 30 || fail 4 "probe failed after the feed (see $OUT/step4.err)"
FRAMES="$(json "$OUT/step4.json" frames)"; SLATE="$(json "$OUT/step4.json" slateFrames)"
[ "$FRAMES" -gt 0 ] && [ "$SLATE" = "$FRAMES" ] \
    || fail 4 "slateFrames $SLATE != frames $FRAMES after the producer stopped (frozen frame?)"
pass 4 "slate after the feed: $FRAMES frames"

# ── 5. extension restart while feeding ──────────────────────────────────────
if [ "$RUN_RESTART" = 1 ]; then
    # Authenticate BEFORE the feed starts: a password prompt must not eat the
    # feed or count against the 3 s recovery budget.
    sudo -v || fail 5 "sudo authentication failed"
    start_feed step5 14
    sleep 3
    EXT_PID="$(pgrep -x "$EXT_EXECUTABLE" | head -1)"
    [ -n "$EXT_PID" ] || fail 5 "no running $EXT_EXECUTABLE process to kill"
    sudo -n kill -9 "$EXT_PID" || fail 5 "sudo kill -9 $EXT_PID failed"
    KILL_AT="$(now_ms)"  # after the kill returned: the recovery clock starts here
    echo "    killed $EXT_EXECUTABLE pid $EXT_PID at $KILL_AT"
    sleep 4
    LIVE_AT="$(awk -v k="$KILL_AT" '/^\[feed\]/ { split($2, a, "="); if (a[2] + 0 > k + 0 && $4 == "status=live") { print a[2]; exit } }' "$OUT/feed-step5.log")"
    [ -n "$LIVE_AT" ] || fail 5 "the feeder never returned to live after the kill (see $OUT/feed-step5.log)"
    [ $((LIVE_AT - KILL_AT)) -le 3000 ] || fail 5 "the feeder took $((LIVE_AT - KILL_AT)) ms to return to live (> 3000)"
    probe step5 2 --fps 30 || fail 5 "probe failed after the restart (see $OUT/step5.err)"
    wait_feed || fail 5 "feeder did not end live (see $OUT/feed-step5.log)"
    FRAMES="$(json "$OUT/step5.json" frames)"; PROGRAM="$(json "$OUT/step5.json" programFrames)"
    awk -v p="$PROGRAM" -v f="$FRAMES" 'BEGIN { exit !(f > 0 && p >= 0.9 * f) }' \
        || fail 5 "programFrames $PROGRAM < 0.9 x frames $FRAMES after the restart"
    pass 5 "live again $((LIVE_AT - KILL_AT)) ms after the kill; program $PROGRAM of $FRAMES"
else
    skip 5 "pass --restart-extension (needs sudo) to kill the extension mid-feed"
fi

# ── 6. the real app feeds the camera ────────────────────────────────────────
if [ "$RUN_APP" = 1 ]; then
    open -a "$APP" || fail 6 "could not launch $APP"
    for _ in $(seq 1 60); do curl -sf "$CONTROL/manifest" >"$OUT/manifest.json" 2>/dev/null && break; sleep 0.5; done
    [ -s "$OUT/manifest.json" ] || fail 6 "the control API at $CONTROL never answered"
    if ! grep -q '"transport.virtualcam.set"' "$OUT/manifest.json"; then
        skip 6 "needs Task 11: the control API has no transport.virtualcam.set yet (app left running)"
    else
        curl -sf -X POST "$CONTROL/invoke" -H 'Content-Type: application/json' \
            -d '{"action":"transport.virtualcam.set","args":[true]}' >"$OUT/invoke-on.json" \
            || fail 6 "transport.virtualcam.set true failed"
        STATUS=""
        for _ in $(seq 1 30); do
            curl -sf "$CONTROL/snapshot" >"$OUT/snapshot-1.json" 2>/dev/null
            STATUS="$(json "$OUT/snapshot-1.json" snapshot.virtualCamera.status)"
            [ "$STATUS" = "live" ] && break
            sleep 0.5
        done
        [ "$STATUS" = "live" ] || fail 6 "virtualCamera never reached live (last: '$STATUS'; $OUT/snapshot-1.json)"
        PUB1="$(json "$OUT/snapshot-1.json" snapshot.virtualCamera.framesPublished)"
        probe step6 3 --fps 30 || fail 6 "probe failed with the app feeding (see $OUT/step6.err)"
        curl -sf "$CONTROL/snapshot" >"$OUT/snapshot-2.json"
        PUB2="$(json "$OUT/snapshot-2.json" snapshot.virtualCamera.framesPublished)"
        FRAMES="$(json "$OUT/step6.json" frames)"; PROGRAM="$(json "$OUT/step6.json" programFrames)"
        awk -v p="$PROGRAM" -v f="$FRAMES" 'BEGIN { exit !(f > 0 && p >= 0.9 * f) }' \
            || fail 6 "programFrames $PROGRAM < 0.9 x frames $FRAMES with the app feeding"
        awk -v a="$PUB1" -v b="$PUB2" 'BEGIN { exit !(b > a) }' \
            || fail 6 "framesPublished did not advance ($PUB1 -> $PUB2)"
        # Quit while probing; every frame later than quit + 1 s must be slate.
        "$PROBE" capture --seconds 4 --json --frames-csv "$OUT/step6-quit.csv" >"$OUT/step6-quit.json" 2>"$OUT/step6-quit.err" &
        PROBE_PID=$!
        sleep 1.5
        QUIT_AT="$(now_ms)"
        osascript -e 'tell application "CoreVideo Pro" to quit' >/dev/null 2>&1 || fail 6 "could not quit the app"
        wait "$PROBE_PID" || fail 6 "probe failed across the quit (see $OUT/step6-quit.err)"
        LATE="$(awk -F, -v q="$QUIT_AT" 'NR > 1 && $1 > q + 1000 { n++ } END { print n + 0 }' "$OUT/step6-quit.csv")"
        LATE_PROGRAM="$(awk -F, -v q="$QUIT_AT" 'NR > 1 && $1 > q + 1000 && $2 == "program" { n++ } END { print n + 0 }' "$OUT/step6-quit.csv")"
        [ "$LATE" -gt 0 ] || fail 6 "no frames captured more than 1 s after the quit"
        [ "$LATE_PROGRAM" = 0 ] || fail 6 "$LATE_PROGRAM program frame(s) more than 1 s after the quit (frozen frame?)"
        pass 6 "app: program $PROGRAM of $FRAMES, framesPublished $PUB1 -> $PUB2; slate within 1 s of quit"
    fi
else
    skip 6 "pass --app to drive the installed app through the control API"
fi

# ── 7. no sample/pixel buffer leaks over a long feed ────────────────────────
echo "    step 7 feeds for $LEAK_SECONDS s under leaks --atExit…"
leaks --atExit -- "$FEED" --seconds "$LEAK_SECONDS" --pattern red >"$OUT/feed-step7.log" 2>&1
judge_leaks_log "$OUT/feed-step7.log"

echo "E2E PASS"
