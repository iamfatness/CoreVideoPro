"""#701 media frame-delivery trace: watch every media source on the bus and say which seam froze.

Reads ONLY the control API (http://127.0.0.1:8011). Never sends input. The app must be running
with Engine on and at least one media clip routed.

    python scripts/qa/media-delivery-trace.py [--seconds 300] [--interval 0.25] [--json PATH]

Per sample, for each row of the core's `mediaSources[]` (#701 fields):
  decodedVideoFrames / lastDecodedAgeMs      the worker pushed a decoded frame
  presentedVideoFrames / lastPresentedAgeMs  the render tick put a NEW frame on air
  videoQueued                                frames prepared and waiting
  decoderRestarts                            new decoder instances after the first

and Program's own delivery counters from `programBuffer` for the same instant.

A stall is reported when a LIVE source's on-air frame has not changed for longer than
--stall-ms (default 1000) and is classified at that instant:
  decoder-stalled       nothing decoded for as long, queue empty        -> the decoder/FFmpeg child
  presentation-stalled  frames decoded recently or queued, none shown   -> the render-side selection
  program-stalled       Program delivery misses rose in the same window -> not media-specific
Exit code 0 means no stall was seen in the window. INFO lines are measurements, not gates.
"""
import argparse
import json
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:8011"


def get(path):
    return json.load(urllib.request.urlopen(BASE + path, timeout=3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--interval", type=float, default=0.25)
    ap.add_argument("--stall-ms", type=float, default=1000)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    samples = []
    stalls = []
    reported = set()
    deadline = time.time() + args.seconds
    last_misses = None
    while time.time() < deadline:
        try:
            doc = get("/snapshot")
        except Exception as exc:  # noqa: BLE001
            print(f"WARN snapshot unavailable: {exc}", file=sys.stderr)
            time.sleep(args.interval)
            continue
        snap = doc.get("snapshot") or {}
        rows = snap.get("mediaSources") or []
        buffer = snap.get("programBuffer") or {}
        misses = (buffer.get("deadlineMisses") or 0) + (buffer.get("underruns") or 0)
        misses_rose = last_misses is not None and misses > last_misses
        last_misses = misses
        now = time.time()
        sample = {"t": now, "programMisses": misses, "sources": []}
        for row in rows:
            if "decodedVideoFrames" not in row:
                print("ERROR this core publishes no #701 trace fields; run a core built from 2026-10-03 or later")
                return 2
            src = {k: row.get(k) for k in (
                "sourceId", "state", "onProgram", "positionMs", "decodedVideoFrames", "lastDecodedAgeMs",
                "presentedVideoFrames", "lastPresentedAgeMs", "videoQueued", "decoderRestarts")}
            sample["sources"].append(src)
            if row.get("state") != "live" or not row.get("onProgram"):
                reported.discard(row.get("sourceId"))
                continue
            presented_age = row.get("lastPresentedAgeMs", -1)
            if presented_age is None or presented_age < args.stall_ms:
                reported.discard(row.get("sourceId"))
                continue
            decoded_age = row.get("lastDecodedAgeMs", -1)
            queued = row.get("videoQueued", 0) or 0
            if misses_rose:
                kind = "program-stalled"
            elif (decoded_age is None or decoded_age < 0 or decoded_age >= args.stall_ms) and queued == 0:
                kind = "decoder-stalled"
            else:
                kind = "presentation-stalled"
            key = row.get("sourceId")
            if key not in reported:
                reported.add(key)
                stall = {"t": now, "sourceId": key, "kind": kind, "lastPresentedAgeMs": presented_age,
                         "lastDecodedAgeMs": decoded_age, "videoQueued": queued,
                         "decoderRestarts": row.get("decoderRestarts"), "programMisses": misses}
                stalls.append(stall)
                print(f"STALL {time.strftime('%H:%M:%S')} {key} {kind}: on-air frame unchanged {presented_age} ms, "
                      f"last decode {decoded_age} ms ago, queued {queued}, restarts {row.get('decoderRestarts')}, "
                      f"program misses {misses}")
        samples.append(sample)
        time.sleep(args.interval)

    live = sorted({s["sourceId"] for smp in samples for s in smp["sources"]})
    print(f"INFO {len(samples)} samples over {args.seconds:.0f}s; media sources seen: {live or 'none'}; stalls: {len(stalls)}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump({"samples": samples, "stalls": stalls}, handle)
        print(f"INFO trace written to {args.json}")
    return 1 if stalls else 0


if __name__ == "__main__":
    sys.exit(main())
