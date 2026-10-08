import copy
import importlib.util
import pathlib
import unittest
from render_work_evidence import judge, compare, WIDTH_NS, CAPACITY, ENDPOINT_RANK_UNCERTAINTY

spec = importlib.util.spec_from_file_location("render_work_cost", pathlib.Path(__file__).with_name("render-work-cost.py"))
cost = importlib.util.module_from_spec(spec); spec.loader.exec_module(cost)


def snapshot(n=0, bucket=500, start=1000000000):
    return {"realtimeEvidence": {"render": {"generation": 1, "completedSlots": n, "workDistribution": {
        "schemaVersion": "render-work-distribution-v1", "enabled": True,
        "bucketWidthNs": WIDTH_NS, "bucketCapacity": CAPACITY,
        "overflowLowerBoundNs": (CAPACITY - 1) * WIDTH_NS, "sampleCount": n,
        "invalidSamples": 0, "scanStartedAtNs": str(start), "scanEndedAtNs": str(start + 1000),
        "bins": [[bucket, n]] if n else []}}}}


class RenderWorkEvidenceTests(unittest.TestCase):
    def test_whole_interval_excludes_warmup_and_retains_resolution(self):
        result = judge(snapshot(100), snapshot(3100, start=2000000000))
        self.assertEqual("PASS", result["result"]); self.assertEqual(3000, result["samples"])
        self.assertEqual({"lowerNs": 500000, "upperNs": 501000}, result["p95"])
        self.assertEqual(ENDPOINT_RANK_UNCERTAINTY, result["endpointRankUncertainty"])

    def test_bad_older_reset_unknown_and_incomplete_evidence_is_rejected(self):
        first, last = snapshot(), snapshot(3000, start=2000000000)
        for field, value in [("enabled", False), ("schemaVersion", "future"), ("invalidSamples", 1),
                             ("sampleCount", 3001), ("bins", [[500, 2999], [500, 1]]),
                             ("bins", [[False, 3000]]), ("scanEndedAtNs", "2016666667")]:
            changed = copy.deepcopy(last); changed["realtimeEvidence"]["render"]["workDistribution"][field] = value
            self.assertEqual("INVALID", judge(first, changed)["result"])
        last["realtimeEvidence"]["render"]["generation"] = 2
        self.assertEqual("INVALID", judge(first, last)["result"])
        self.assertEqual("INVALID", judge(snapshot(3100), snapshot(3000, start=2000000000))["result"])
        self.assertEqual("INVALID", judge(snapshot(), snapshot(10, start=2000000000))["result"])
        self.assertEqual("INVALID", judge({}, {})["result"])

    def test_overflow_cannot_invent_a_finite_quantile(self):
        self.assertEqual("INVALID", judge(snapshot(), snapshot(3000, bucket=CAPACITY-1, start=2000000000))["result"])

    def test_rank_uncertainty_retains_boundary_outliers(self):
        last = snapshot(3000, start=2000000000)
        last["realtimeEvidence"]["render"]["workDistribution"]["bins"] = [[500, 2850], [900, 150]]
        result = judge(snapshot(), last)
        self.assertEqual({"lowerNs": 500000, "upperNs": 901000}, result["p95"])

    def test_cost_uses_conservative_bounds_and_rejects_missing_or_malformed_measurements(self):
        baseline = judge(snapshot(), snapshot(3000, start=2000000000))
        self.assertEqual("PASS", compare(baseline, baseline)["result"])
        traced = judge(snapshot(), snapshot(3000, bucket=510, start=2000000000))
        self.assertEqual("FAIL", compare(baseline, traced)["result"])
        self.assertEqual("INVALID", compare({}, baseline)["result"])
        malformed = copy.deepcopy(baseline); malformed["p95"]["upperNs"] = float("nan")
        self.assertEqual("INVALID", compare(baseline, malformed)["result"])

    def test_matched_cost_rejects_binary_flag_hardware_and_delivery_mismatch(self):
        distribution = judge(snapshot(), snapshot(3000, start=2000000000))
        manifest = {k: "same" for k in ["harnessSha256", "driverSha256", "renderWorkJudgeSha256", "sourceCommit", "buildConfiguration",
                    "coreSha256", "fakeSha256", "programScene", "programRoutes", "sourceFormats", "output", "hardware"]}
        manifest.update(flags={"COREVIDEO_QA_RENDER_WORK_DISTRIBUTION": "1"}, duration=45, pairs=1, warmup=15,
                        programBufferFrames=2, deliveryTraceRequested=False, verboseDiagnostics=True, trials=[])
        for isolated in (False, True):
            manifest["trials"].append({"label": str(isolated), "isolated": isolated,
                "programBufferVerdict": "PASS", "recordingVerdict": "PASS", "sampledSourceAdmissionVerdict": "PASS",
                "renderWorkDistribution": distribution, "deliveryTrace": {"result": "PASS"}})
        traced = copy.deepcopy(manifest); traced["deliveryTraceRequested"] = True
        self.assertEqual("PASS", cost.judge(manifest, traced)["result"])
        for field in ("coreSha256", "hardware", "flags", "programRoutes", "verboseDiagnostics"):
            changed = copy.deepcopy(traced); changed[field] = "different"
            self.assertEqual("INVALID", cost.judge(manifest, changed)["result"])
        changed = copy.deepcopy(traced); changed["verboseDiagnostics"] = False
        self.assertEqual("INVALID", cost.judge(manifest, changed)["result"])
        older = copy.deepcopy(manifest); older.pop("verboseDiagnostics")
        self.assertEqual("INVALID", cost.judge(older, traced)["result"])
        for verdict in ("programBufferVerdict", "recordingVerdict", "sampledSourceAdmissionVerdict"):
            changed = copy.deepcopy(traced); changed["trials"][0][verdict] = "FAIL"
            self.assertEqual("INVALID", cost.judge(manifest, changed)["result"])
        changed = copy.deepcopy(traced); changed["trials"][0]["deliveryTrace"]["result"] = "FAIL"
        self.assertEqual("INVALID", cost.judge(manifest, changed)["result"])
        changed = copy.deepcopy(traced); changed["trials"].pop()
        self.assertEqual("INVALID", cost.judge(manifest, changed)["result"])


if __name__ == "__main__": unittest.main()
