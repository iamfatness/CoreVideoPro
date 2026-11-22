import importlib.util, os, unittest
HERE = os.path.dirname(__file__)
spec = importlib.util.spec_from_file_location("proof", os.path.join(HERE, "mac-grade-preview-proof.py"))
proof = importlib.util.module_from_spec(spec); spec.loader.exec_module(proof)

PROGRAM = (200, 180, 120)
BLACK = (0, 0, 0)


class Judge(unittest.TestCase):
    def verdict(self, **overrides):
        args = dict(program_before=PROGRAM, graded=BLACK, program_during=PROGRAM, compare=PROGRAM,
                    program_after=PROGRAM, scope_bright_pixels=100)
        args.update(overrides)
        return proof.judge(**args)

    def test_pass(self):
        self.assertEqual(self.verdict()["verdict"], "pass")

    def test_graded_preview_not_black_fails(self):
        v = self.verdict(graded=(10, 10, 60))
        self.assertEqual(v["verdict"], "fail"); self.assertIn("not black", " ".join(v["reasons"]))

    def test_draft_leaking_to_program_fails(self):
        v = self.verdict(program_during=BLACK)
        self.assertEqual(v["verdict"], "fail"); self.assertIn("Program changed during", " ".join(v["reasons"]))

    def test_compare_must_match_program_within_tolerance(self):
        self.assertEqual(self.verdict(compare=(206, 180, 120))["verdict"], "pass")
        self.assertEqual(self.verdict(compare=(207, 180, 120))["verdict"], "fail")

    def test_empty_scope_fails(self):
        self.assertEqual(self.verdict(scope_bright_pixels=0)["verdict"], "fail")

    def test_dark_program_is_a_harness_failure(self):
        self.assertEqual(self.verdict(program_before=(20, 20, 20))["verdict"], "harness")

    def test_unreadable_surface_is_a_harness_failure(self):
        self.assertEqual(self.verdict(compare=None)["verdict"], "harness")


if __name__ == "__main__":
    unittest.main()
