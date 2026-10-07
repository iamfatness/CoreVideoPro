import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("delivery_trace", pathlib.Path(__file__).with_name("delivery-trace.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DeliveryTraceJudgeTests(unittest.TestCase):
    def fixture(self, lost=0, complete=1):
        events = []
        for identity in range(121):
            timestamp = 1000 + round(identity * 1000000 / 60)
            for stage, offset in [(3, 0), (4, 10), (5, 20)]:
                events.append(module.EVENT.pack(77, 0, 1, identity, -1, timestamp + offset, 0, 9, stage, 0))
        header = module.HEADER.pack(b"CVTRACE1", 1, module.EVENT.size, 1000000, 77,
            len(events), lost, 0, complete, 1000, 2100000)
        return header, events

    def judge(self, header, events):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "trace.bin"
            path.write_bytes(header + b"".join(events))
            return module.judge(path, 0, 2)

    def test_accepts_exact_program_boundary_join(self):
        self.assertEqual(self.judge(*self.fixture())["result"], "PASS")

    def test_loss_never_certifies_a_boundary(self):
        self.assertEqual(self.judge(*self.fixture(lost=1))["result"], "FAIL")

    def test_partial_and_unfinalized_captures_are_invalid(self):
        header, events = self.fixture()
        with self.assertRaises(ValueError): self.judge(header, events[:-1])
        with self.assertRaises(ValueError): self.judge(*self.fixture(complete=0))

    def test_missing_gpu_completion_cannot_pass(self):
        header, events = self.fixture()
        index = 60 * 3 + 1
        row = list(module.EVENT.unpack(events[index])); row[-2] = 1
        events[index] = module.EVENT.pack(*row)
        self.assertEqual(self.judge(header, events)["result"], "FAIL")

    def test_missing_submission_even_at_window_edge_cannot_pass(self):
        header, events = self.fixture()
        row = list(module.EVENT.unpack(events[0])); row[-2] = 1
        events[0] = module.EVENT.pack(*row)
        self.assertEqual(self.judge(header, events)["result"], "FAIL")

    def test_duplicate_program_stage_and_wrong_layout_fail(self):
        for replacement in ["duplicate", "layout"]:
            header, events = self.fixture()
            index = 60 * 3 + 1
            row = list(module.EVENT.unpack(events[index]))
            if replacement == "duplicate": row[3] = 59
            else: row[7] = 11
            events[index] = module.EVENT.pack(*row)
            self.assertEqual(self.judge(header, events)["result"], "FAIL")

    def test_epoch_stage_and_timestamp_corruption_are_invalid(self):
        for field, value in [(0, 88), (8, 99), (5, 999999999)]:
            header, events = self.fixture()
            row = list(module.EVENT.unpack(events[3])); row[field] = value
            events[3] = module.EVENT.pack(*row)
            with self.assertRaises(ValueError): self.judge(header, events)

    def test_selected_source_requires_exact_preceding_gpu_identity(self):
        header, events = self.fixture()
        values = list(module.HEADER.unpack(header)); values[5] += 3
        ready = module.EVENT.pack(77, 42, 7, -1, 18, 1000, 50, 0, 1, 0)
        requested = module.EVENT.pack(77, 42, 7, 0, 18, 1005, 50, 9, 7, 0)
        selected = module.EVENT.pack(77, 42, 7, 0, 18, 1010, 50, 9, 2, 1)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "trace.bin"
            path.write_bytes(module.HEADER.pack(*values) + ready + requested + selected + b"".join(events))
            self.assertEqual(module.judge(path, 0, 2, require_source_ready=True)["result"], "PASS")
            bad = module.EVENT.pack(77, 42, 7, -1, 19, 1000, 50, 0, 1, 0)
            path.write_bytes(module.HEADER.pack(*values) + bad + requested + selected + b"".join(events))
            self.assertEqual(module.judge(path, 0, 2, require_source_ready=True)["result"], "FAIL")

    def test_missing_expected_source_cannot_be_hidden_by_program_progress(self):
        header, events = self.fixture()
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "trace.bin"
            path.write_bytes(header + b"".join(events))
            self.assertEqual(module.judge(path, 0, 2, expected_source_ids=["101"])["result"], "FAIL")

    def test_50fps_identity_progress_cannot_pass(self):
        header, events = self.fixture()
        for index, event in enumerate(events):
            row = list(module.EVENT.unpack(event))
            row[5] = 1000 + (index // 3) * 20000 + (index % 3) * 10
            events[index] = module.EVENT.pack(*row)
        values = list(module.HEADER.unpack(header)); values[-1] = 2500000
        self.assertEqual(self.judge(module.HEADER.pack(*values), events)["result"], "FAIL")


if __name__ == "__main__": unittest.main()
