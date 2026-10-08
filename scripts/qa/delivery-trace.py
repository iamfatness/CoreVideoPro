#!/usr/bin/env python3
"""Judge the explicitly captured core boundaries; no receiver/display or acquisition claims."""
import argparse
import json
import pathlib
import struct

HEADER = struct.Struct("<8sIIQQQQQQqq")
EVENT = struct.Struct("<QQQqqqqQII")
STAGES = {1: "source-gpu-ready", 2: "source-draw-submitted", 3: "program-submitted",
          4: "program-gpu-ready", 5: "program-delivered", 6: "program-miss", 7: "source-requested", 8: "source-upload-started",
          9: "source-upload-submitted", 10: "source-upload-refused"}


def judge(path, warmup=15, minimum=30, start_ticks=None, end_ticks=None, require_source_ready=False, expected_source_ids=None):
    errors = []
    if not 0 <= warmup <= 30 or minimum < 1:
        raise ValueError("warmup must be 0..30 and minimum >=1")
    if pathlib.Path(path).stat().st_size > 256 * 1024 * 1024:
        raise ValueError("export exceeds declared capacity")
    with pathlib.Path(path).open("rb") as source:
        raw = source.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError("partial header")
        magic, version, size, frequency, epoch, count, lost, failures, complete, started, ended = HEADER.unpack(raw)
        if magic != b"CVTRACE1" or version != 1 or size != EVENT.size:
            raise ValueError("unknown format")
        if frequency <= 0 or epoch <= 0 or ended <= started or complete != 1:
            raise ValueError("unfinalized or invalid capture clock/epoch")
        if pathlib.Path(path).stat().st_size != HEADER.size + count * EVENT.size:
            raise ValueError("partial/trailing capture")
        if lost or failures:
            errors.append("trace loss or export failure invalidates boundary acceptance")
        baseline = start_ticks if start_ticks is not None else started + warmup * frequency
        end = ended if end_ticks is None else end_ticks
        if not started <= baseline < end <= ended:
            raise ValueError("measurement window outside finalized capture")
        stages, deliveries, misses, sources = {}, [], 0, {}
        gpu_ready, selected, requested, selection_age, drawn = {}, [], {}, [], {}
        upload_started, upload_submitted, upload_duration, refusals, source_metrics = {}, {}, {}, {}, {}
        for _ in range(count):
            record = source.read(EVENT.size)
            if len(record) != EVENT.size:
                raise ValueError("partial event")
            event_epoch, tag, source_epoch, program, frame, timestamp, observed, layout, stage, reason = EVENT.unpack(record)
            if event_epoch != epoch or stage not in STAGES or reason not in range(5) or not started <= timestamp <= ended:
                raise ValueError("malformed event identity/time/stage")
            if stage in (3, 4, 5):
                key = (source_epoch, program, stage)
                if source_epoch == 0 or program < 0 or key in stages:
                    errors.append("duplicate or invalid Program boundary identity")
                stages[key] = (timestamp, layout)
                if stage == 5 and baseline <= timestamp <= end:
                    deliveries.append((source_epoch, program, timestamp))
            elif stage == 7:
                requested[(program, tag, source_epoch)] = (frame, observed, timestamp)
            elif stage == 1:
                key = (tag, source_epoch, frame)
                gpu_ready[key] = min(timestamp, gpu_ready.get(key, timestamp))
            elif stage == 8:
                upload_started[(tag, source_epoch, frame)] = timestamp
            elif stage == 9:
                key = (tag, source_epoch, frame)
                upload_submitted[key] = timestamp
                if key in upload_started:
                    upload_duration[key] = timestamp - upload_started[key]
            elif stage == 10 and baseline <= timestamp <= end:
                refusals[tag] = refusals.get(tag, 0) + 1
            elif stage == 6 and baseline <= timestamp <= end:
                misses += 1
            elif stage == 2:
                drawn.setdefault(program, []).append((tag, source_epoch, frame, observed, timestamp, reason))
        expected_tags = set()
        for identity in expected_source_ids or []:
            tag = 14695981039346656037 ^ epoch
            for value in identity.encode("utf8"):
                tag = ((tag ^ value) * 1099511628211) & 0xffffffffffffffff
            expected_tags.add(tag)
        for generation, program, delivered_time in deliveries:
            ingredients = drawn.get(program, [])
            if expected_tags and {row[0] for row in ingredients} != expected_tags:
                errors.append("delivered Program missing its expected drawn sources")
            for tag, source_epoch, frame, observed, timestamp, reason in ingredients:
                if reason == 3 or source_epoch == 0 or frame < 0 or observed <= 0 or tag == 0:
                    errors.append("unavailable or unattributable selected source")
                sources.setdefault(tag, set()).add(frame)
                selected.append(((tag, source_epoch, frame), timestamp))
                metrics = source_metrics.setdefault(tag, {"draws": 0, "held": 0, "ages": [], "readyAges": [], "keys": set()})
                metrics["draws"] += 1
                metrics["held"] += reason == 2
                metrics["keys"].add((tag, source_epoch, frame))
                if (tag, source_epoch, frame) in gpu_ready:
                    metrics["readyAges"].append((timestamp - gpu_ready[(tag, source_epoch, frame)]) * 1000 / frequency)
                wanted = requested.get((program, tag, source_epoch))
                if wanted is None or wanted[0] < frame or wanted[1] < observed or wanted[2] > timestamp:
                    errors.append("draw missing matching requested source identity")
                else:
                    selection_age.append((wanted[1] - observed) / 10000)
                    metrics["ages"].append((wanted[1] - observed) / 10000)
        if require_source_ready:
            for key, timestamp in selected:
                if key not in gpu_ready or gpu_ready[key] > timestamp:
                    errors.append("selected source has no preceding exact GPU completion")
        # Arrival order between threads is not timestamp order. Join all exact
        # stage keys, including submissions before the measured warmup edge.
        qualified = []
        for generation, program, timestamp in deliveries:
            submitted = stages.get((generation, program, 3))
            ready = stages.get((generation, program, 4))
            if submitted is None or ready is None or not submitted[0] <= ready[0] <= timestamp:
                errors.append("delivery missing preceding submission/GPU completion")
            elif submitted[1] != ready[1] or ready[1] != stages[(generation, program, 5)][1]:
                errors.append("layout attribution changed between Program stages")
            qualified.append((generation, program, timestamp))
        qualified.sort(key=lambda row: row[2])
        gaps = sum(max(0, b[1] - a[1] - 1) for a, b in zip(qualified, qualified[1:]) if a[0] == b[0])
        reordered = sum(b[1] <= a[1] for a, b in zip(qualified, qualified[1:]) if a[0] == b[0])
        seconds = (qualified[-1][2] - qualified[0][2]) / frequency if len(qualified) > 1 else 0
        if seconds + 1 / 60 < minimum:
            errors.append("insufficient measured Program interval")
        if gaps or reordered or misses:
            errors.append("Program gaps/reordering/misses")
        # Counter identity alone could falsely pass 50fps under a 60fps label.
        if abs(len(qualified) - (round(seconds * 60) + 1)) > 1:
            errors.append("Program delivery differs from required 60/1 cadence")
        selection_age.sort()
        percentile = lambda p: selection_age[min(len(selection_age) - 1, int(p * len(selection_age)))] if selection_age else None
        def distribution(values):
            values = sorted(values)
            return {"samples": len(values), "p50": values[int(.5 * len(values))] if values else None,
                    "p95": values[int(.95 * len(values))] if values else None, "maximum": values[-1] if values else None}
        per_source = {}
        for tag, metrics in source_metrics.items():
            keys = metrics["keys"]
            per_source[str(tag)] = {"draws": metrics["draws"], "held": metrics["held"], "uniqueSelectedFrames": len(keys),
                "uploadRefusals": refusals.get(tag, 0),
                "uploadCallMs": distribution([upload_duration[key] * 1000 / frequency for key in keys if key in upload_duration]),
                "submissionToObservedGpuReadyMs": distribution([(gpu_ready[key] - upload_submitted[key]) * 1000 / frequency for key in keys if key in gpu_ready and key in upload_submitted]),
                "observedReadyToDrawMs": distribution(metrics["readyAges"]),
                "selectionObservationAgeMs": distribution(metrics["ages"])}
        return {"schema": "delivery-trace-verdict-v1", "result": "FAIL" if errors else "PASS",
                "exported": count, "lost": lost, "exportFailures": failures,
                "measuredSeconds": seconds, "measurementStartTicks": baseline, "measurementEndTicks": end, "delivered": len(qualified), "gaps": gaps,
                "reordered": reordered, "misses": misses,
                "sourceSelectionObservationAgeMs": {"samples": len(selection_age), "p50": percentile(.5), "p95": percentile(.95), "maximum": selection_age[-1] if selection_age else None, "scope": "Requested CPU selection versus actual drawn source observation; not acquisition-to-receiver content latency"},
                "sourceGpuCompletionRequired": require_source_ready,
                "sourceStageDistributions": per_source,
                "selectedSources": {str(tag): len(frames) for tag, frames in sources.items()},
                "errors": sorted(set(errors)), "releaseQualification": "MISSING_EVIDENCE",
                "scope": "Core source draw submission and buffered Program GPU completion/delivery only. Camera, display, A/V, actual acquisition/content latency and fleet remain unobserved."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--minimum", type=float, default=30)
    args = parser.parse_args()
    try:
        report = judge(args.path, args.warmup, args.minimum)
    except Exception as error:
        report = {"result": "INVALID", "error": str(error)}
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["result"] == "PASS" else 1)
