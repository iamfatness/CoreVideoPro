import importlib.util, os, unittest
HERE = os.path.dirname(__file__)
spec = importlib.util.spec_from_file_location("proof", os.path.join(HERE, "mac-source-grade-proof.py"))
proof = importlib.util.module_from_spec(spec); spec.loader.exec_module(proof)

def plane(y, n=1000):
    return bytes([y]) * n

class Judge(unittest.TestCase):
    def test_pass_when_only_the_graded_half_goes_black(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(131)] * 3)
        self.assertEqual(v["verdict"], "pass", v)

    def test_fail_when_the_neighbour_also_goes_black(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(16)] * 3)
        self.assertEqual(v["verdict"], "fail")
        self.assertIn("ungraded neighbour changed", " ".join(v["reasons"]))

    def test_fail_when_the_graded_source_did_not_change(self):
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(120)] * 3, [plane(130)] * 3)
        self.assertEqual(v["verdict"], "fail")
        self.assertIn("graded source did not change", " ".join(v["reasons"]))

    def test_one_bright_frame_in_the_graded_window_fails(self):
        # Fixture corrected: the window MEAN must stay above 0.98 so only a per-frame rule can
        # catch the one half-bright frame (59 black + 1 half black = mean 0.992). The plan's
        # [16, 16, 120] window has mean 0.67 and failed under the "mean" mutation it names too.
        half = plane(16, 500) + plane(120, 500)
        v = proof.judge([plane(120)] * 3, [plane(130)] * 3, [plane(16)] * 59 + [half], [plane(130)] * 3)
        self.assertEqual(v["verdict"], "fail")

    def test_harness_when_the_before_window_is_dark(self):
        v = proof.judge([plane(16)] * 3, [plane(130)] * 3, [plane(16)] * 3, [plane(130)] * 3)
        self.assertEqual(v["verdict"], "harness")

    def test_harness_when_a_window_is_empty(self):
        self.assertEqual(proof.judge([], [plane(130)], [plane(16)], [plane(130)])["verdict"], "harness")

    def test_black_fraction_threshold_is_inclusive(self):
        self.assertEqual(proof.black_fraction(plane(24)), 1.0)
        self.assertEqual(proof.black_fraction(plane(25)), 0.0)

if __name__ == "__main__":
    unittest.main()
