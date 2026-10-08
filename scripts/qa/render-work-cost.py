#!/usr/bin/env python3
"""Read-only matched trace-on/off CPU p95 gate. Never certifies a release."""
import argparse
import json
import pathlib
from render_work_evidence import compare


def judge(reference, traced):
    try:
        if reference.get("deliveryTraceRequested") is not False or traced.get("deliveryTraceRequested") is not True:
            raise ValueError("explicit trace-off reference and trace-on candidate required")
        keys = ["harnessSha256", "driverSha256", "renderWorkJudgeSha256", "sourceCommit", "buildConfiguration",
                "flags", "coreSha256", "fakeSha256", "duration", "pairs", "warmup", "programBufferFrames",
                "programScene", "programRoutes", "sourceFormats", "output", "hardware"]
        if any(k not in reference or k not in traced or reference[k] != traced[k] for k in keys):
            raise ValueError("unmatched workload/binary/collector/hardware manifests")
        if reference["flags"].get("COREVIDEO_QA_RENDER_WORK_DISTRIBUTION") != "1" or reference["hardware"] == "MISSING_EVIDENCE":
            raise ValueError("missing collector or hardware identity")
        if type(reference["pairs"]) is not int or reference["pairs"] < 1 or reference["duration"] < 30:
            raise ValueError("short/incomplete comparison")
        rows = []
        def index(report):
            trials = report["trials"]
            values = {r["label"]: r for r in trials}
            if len(values) != len(trials) or len(trials) != report["pairs"] * 2:
                raise ValueError("missing/duplicate trial")
            return values
        a, b = index(reference), index(traced)
        if a.keys() != b.keys(): raise ValueError("missing matched trial")
        for label in sorted(a):
            x, y = a[label], b[label]
            if x["isolated"] != y["isolated"]: raise ValueError("monitor mode changed")
            for trial in (x, y):
                if trial.get("programBufferVerdict") != "PASS" or trial.get("recordingVerdict") != "PASS" or trial.get("sampledSourceAdmissionVerdict") == "FAIL":
                    raise ValueError("delivery/recording/source comparison failed")
            if y.get("deliveryTrace", {}).get("result") != "PASS": raise ValueError("lossy/incomplete candidate trace")
            rows.append({"trial": label, **compare(x.get("renderWorkDistribution", {}), y.get("renderWorkDistribution", {}))})
        result = "INVALID" if any(r["result"] == "INVALID" for r in rows) else "FAIL" if any(r["result"] == "FAIL" for r in rows) else "PASS"
        return {"result": result, "trials": rows, "releaseQualification": "MISSING_EVIDENCE",
                "scope": "Matched CPU p95 trace overhead only; source/content latency, receiver/display, A/V and fleet remain unqualified."}
    except (KeyError, TypeError, ValueError) as error:
        return {"result": "INVALID", "error": str(error), "releaseQualification": "MISSING_EVIDENCE"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=pathlib.Path)
    parser.add_argument("traced", type=pathlib.Path)
    args = parser.parse_args()
    try:
        values = []
        for path in (args.reference, args.traced):
            if path.stat().st_size > 4 * 1024 * 1024: raise ValueError("unbounded report")
            values.append(json.loads(path.read_text()))
        result = judge(*values)
    except (OSError, ValueError) as error:
        result = {"result": "INVALID", "error": str(error)}
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["result"] == "PASS" else 1)
