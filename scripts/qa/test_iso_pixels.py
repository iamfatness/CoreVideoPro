import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('iso_pixels', pathlib.Path(__file__).with_name('iso-pixel-verdict.py'))
iso = importlib.util.module_from_spec(spec)
spec.loader.exec_module(iso)


class IsoPixelsTest(unittest.TestCase):
    def test_source_cadence_and_recording_padding_do_not_require_fake_60fps(self):
        expected = [100, 102, 108, 110]
        result = iso.compare_identities(expected, [90, 90, 94, *expected, 114, 114])
        self.assertTrue(result['isoPixelArrivalFidelityPassed'])
        self.assertEqual(result['recordedSamples'], 4)

    def test_missing_repeat_reorder_invalid_and_wrong_picture_fail(self):
        for decoded in ([100, 108, 110], [100, 102, 102, 108, 110], [100, 108, 102, 110],
                        [100, None, 108, 110], [100, 104, 108, 110], [102, 108, 110],
                        [100, 102, 108], [100, 102, 108, 110, 114, 108]):
            with self.subTest(decoded=decoded):
                self.assertFalse(iso.compare_identities([100, 102, 108, 110], decoded)['isoPixelArrivalFidelityPassed'])

    def write_trace(self, path, *, lost=0, complete=1, second_epoch=5, source='zoom:1000', dimensions=(1920, 1080), v2=False):
        tag = iso.source_tag(source, 777)
        records = [iso.EVENT.pack(777, tag, epoch, -1, frame, at, at * 10000,
                                  dimensions[0] << 32 | dimensions[1], 11, 0)
                   for epoch, frame, at in [(5, 100, 1500), (second_epoch, 102, 2500), (5, 108, 3500)]]
        if v2: records = [iso.EVENT_V2.pack(*iso.EVENT.unpack(record), 0, 0) for record in records]
        path.write_bytes(iso.HEADER.pack(b'CVTRACE2' if v2 else b'CVTRACE1', 2 if v2 else 1, iso.EVENT_V2.size if v2 else iso.EVENT.size, 1000, 777,
                                       len(records), lost, 0, complete, 1000, 4000) + b''.join(records))

    def test_trace_requires_complete_nonlossy_same_owner_1080p_source(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'trace.bin'
            self.write_trace(path)
            frames, coverage = iso.expected_arrivals(path, 'zoom:1000', 1000, 4000, 3)
            self.assertEqual(frames, [100, 102, 108])
            self.assertEqual(coverage['sourceArrivalFps'], 1)
            self.write_trace(path, v2=True)
            self.assertEqual(iso.expected_arrivals(path, 'zoom:1000', 1000, 4000, 3)[0], frames)
            for mutation in ({'lost': 1}, {'complete': 0}, {'second_epoch': 6},
                             {'source': 'zoom:2000'}, {'dimensions': (320, 180)}):
                self.write_trace(path, **mutation)
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    iso.expected_arrivals(path, 'zoom:1000', 1000, 4000, 3)

    def test_short_truncated_and_outside_interval_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'trace.bin'
            self.write_trace(path)
            with self.assertRaises(ValueError): iso.expected_arrivals(path, 'zoom:1000', 1000, 4000, 1800)
            with self.assertRaises(ValueError): iso.expected_arrivals(path, 'zoom:1000', 0, 4000, 3)
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(ValueError): iso.expected_arrivals(path, 'zoom:1000', 1000, 4000, 3)


if __name__ == '__main__':
    unittest.main()
