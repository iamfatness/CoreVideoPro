"""Independent CPU-work quantile evidence, with explicit histogram/scan uncertainty."""
import math

WIDTH_NS = 1000
CAPACITY = 32769
FRAME_NS = 1000000000 / 60
ENDPOINT_RANK_UNCERTAINTY = 12  # Catch-up plus boundary/reporting handoff at both endpoints.


def integer(value):
    return type(value) is int and 0 <= value <= 9007199254740991


def read(snapshot):
    render = snapshot["realtimeEvidence"]["render"]
    value = render["workDistribution"]
    if value.get("schemaVersion") != "render-work-distribution-v1" or value.get("enabled") is not True:
        raise ValueError("missing explicit render-work collector")
    if value.get("bucketWidthNs") != WIDTH_NS or value.get("bucketCapacity") != CAPACITY or value.get("overflowLowerBoundNs") != (CAPACITY - 1) * WIDTH_NS:
        raise ValueError("unsupported histogram geometry")
    if value.get("invalidSamples") != 0:
        raise ValueError("invalid work samples")
    bins = value.get("bins")
    if not isinstance(bins, list) or len(bins) > CAPACITY:
        raise ValueError("unbounded histogram")
    counts = {}
    previous = -1
    for row in bins:
        if not isinstance(row, list) or len(row) != 2 or not all(integer(v) for v in row) or row[0] <= previous or row[0] >= CAPACITY or row[1] == 0:
            raise ValueError("malformed/duplicate histogram bin")
        previous = row[0]; counts[row[0]] = row[1]
    if not integer(value.get("sampleCount")) or sum(counts.values()) != value["sampleCount"]:
        raise ValueError("sample accounting mismatch")
    stamps = [value.get(k) for k in ("scanStartedAtNs", "scanEndedAtNs")]
    if any(not isinstance(v, str) or not v.isdecimal() for v in stamps):
        raise ValueError("missing raw scan clocks")
    started, ended = map(int, stamps)
    if not 0 <= ended - started < FRAME_NS:
        raise ValueError("scan uncertainty exceeds one frame")
    if not integer(render.get("generation")) or not integer(render.get("completedSlots")):
        raise ValueError("missing worker generation/slot accounting")
    return counts, started, ended, render


def judge(first, last, minimum=1200):
    try:
        a, a_start, a_end, old = read(first)
        b, b_start, b_end, new = read(last)
        if new["generation"] != old["generation"] or b_start <= a_end:
            raise ValueError("worker generation changed or interval invalid")
        counts = {k: b.get(k, 0) - a.get(k, 0) for k in a.keys() | b.keys()}
        if any(v < 0 for v in counts.values()):
            raise ValueError("histogram reset")
        count = sum(counts.values())
        if count < minimum or abs(count - (new["completedSlots"] - old["completedSlots"])) > ENDPOINT_RANK_UNCERTAINTY:
            raise ValueError("missing render samples or slot accounting")
        # Three-frame catch-up permits four overdue deadlines at equality.
        # Add an ordinary slot and reporting handoff: six ranks per endpoint.
        def quantile(q):
            target = math.ceil(q * count)
            def bucket(rank):
                accumulated = 0
                for index, n in sorted(counts.items()):
                    accumulated += n
                    if accumulated >= rank: return index
                raise ValueError("missing quantile")
            low = bucket(max(1, target - ENDPOINT_RANK_UNCERTAINTY)); high = bucket(min(count, target + ENDPOINT_RANK_UNCERTAINTY))
            if high == CAPACITY - 1:
                raise ValueError("quantile exceeds finite measurement range")
            return {"lowerNs": low * WIDTH_NS, "upperNs": (high + 1) * WIDTH_NS}
        return {"result": "PASS", "samples": count, "bucketWidthNs": WIDTH_NS, "endpointRankUncertainty": ENDPOINT_RANK_UNCERTAINTY,
                "p95": quantile(.95), "p99": quantile(.99), "p999": quantile(.999),
                "scanSpanNs": [a_end - a_start, b_end - b_start],
                "scope": "CPU render work only; no GPU, source/content latency or presentation qualification."}
    except (AttributeError, KeyError, TypeError, ValueError) as error:
        return {"result": "INVALID", "error": str(error)}


def compare(reference, traced):
    if not isinstance(reference, dict) or not isinstance(traced, dict):
        return {"result": "INVALID", "error": "distribution summaries must be objects"}
    if reference.get("result") != "PASS" or traced.get("result") != "PASS":
        return {"result": "INVALID", "error": "both whole-interval distributions are required"}
    try:
        for value in (reference, traced):
            if value.get("bucketWidthNs") != WIDTH_NS or value.get("endpointRankUncertainty") != ENDPOINT_RANK_UNCERTAINTY:
                raise ValueError("missing measurement uncertainty")
            for name in ("p95", "p99", "p999"):
                bounds = value[name]
                if not integer(bounds["lowerNs"]) or not integer(bounds["upperNs"]) or not bounds["lowerNs"] < bounds["upperNs"] <= (CAPACITY - 1) * WIDTH_NS:
                    raise ValueError("invalid quantile interval")
        lower = reference["p95"]["lowerNs"]
        upper = traced["p95"]["upperNs"]
    except (KeyError, TypeError, ValueError) as error:
        return {"result": "INVALID", "error": str(error)}
    if lower <= 0:
        return {"result": "INVALID", "error": "reference p95 has no positive lower bound"}
    maximum = upper / lower - 1
    return {"result": "PASS" if maximum < .01 else "FAIL", "maximumP95RegressionFraction": maximum,
            "limitFraction": .01, "scope": "Conservative matched CPU p95 trace-cost gate; requires matching manifests, not a release verdict."}
