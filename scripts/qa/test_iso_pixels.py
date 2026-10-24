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
        self.assertEqual((result['missing'], result['repeated']), (0, 0))

    def test_missing_and_repeated_counts_are_per_identity(self):
        result = iso.compare_identities([100, 102, 108, 110], [100, 102, 102, 110])
        self.assertFalse(result['isoPixelArrivalFidelityPassed'])
        self.assertEqual((result['missing'], result['repeated']), (1, 1))

    def test_missing_repeat_reorder_invalid_and_wrong_picture_fail(self):
        for decoded in ([100, 108, 110], [100, 102, 102, 108, 110], [100, 108, 102, 110],
                        [100, None, 108, 110], [100, 104, 108, 110], [102, 108, 110],
                        [100, 102, 108], [100, 102, 108, 110, 114, 108]):
            with self.subTest(decoded=decoded):
                self.assertFalse(iso.compare_identities([100, 102, 108, 110], decoded)['isoPixelArrivalFidelityPassed'])

    # #1030 held frames. Times are presentation seconds; 30 fps frame period.
    PERIOD = 1 / 30

    def test_healthy_file_passes_with_no_held_frames(self):
        decoded = [(100 + i, i * self.PERIOD) for i in range(4)]
        result = iso.compare_identities([100, 101, 102, 103], decoded, 0, self.PERIOD)
        self.assertTrue(result['isoPixelArrivalFidelityPassed'])
        self.assertEqual((result['heldFrames'], result['heldExcused'], result['repeated']), (0, 0, 0))

    def test_a_source_stall_without_holds_loses_no_arrival(self):
        # Pins the drill's behaviour before held frames: the trace expects only
        # OFFERED arrivals, so a 2 s video stall (no arrivals) is not "missing";
        # the stall shows only in coverage.maximumSourceArrivalGapMs.
        decoded = [(100, 0.0), (101, self.PERIOD), (102, 2.0 + self.PERIOD)]
        result = iso.compare_identities([100, 101, 102], decoded, 0, self.PERIOD)
        self.assertTrue(result['isoPixelArrivalFidelityPassed'])
        self.assertEqual((result['missing'], result['repeated']), (0, 0))

    def test_stall_with_the_writer_hold_pattern_passes_and_reports_held(self):
        # The last picture (101) is held once per second while the video is
        # stalled; the first hold is 1 s after it, minus up to one frame period.
        decoded = [(100, 0.0), (101, self.PERIOD), (101, 1.0), (101, 2.02), (101, 3.03), (102, 3.5)]
        result = iso.compare_identities([100, 101, 102], decoded, 3, self.PERIOD)
        self.assertTrue(result['isoPixelArrivalFidelityPassed'], result)
        self.assertEqual((result['heldExcused'], result['holdsMatched'], result['repeated']), (3, True, 0))

    def test_a_repeat_that_is_not_a_hold_fails(self):
        # Adjacent duplicate (one frame period later) is a real repeat even when
        # the writer reported a hold.
        adjacent = [(100, 0.0), (101, self.PERIOD), (101, 2 * self.PERIOD), (102, 3 * self.PERIOD)]
        result = iso.compare_identities([100, 101, 102], adjacent, 1, self.PERIOD)
        self.assertFalse(result['isoPixelArrivalFidelityPassed'])
        self.assertEqual((result['repeated'], result['heldExcused']), (1, 0))
        # A repeat of an EARLIER identity is never a hold.
        earlier = [(100, 0.0), (101, self.PERIOD), (100, 1.5), (102, 1.6)]
        self.assertFalse(iso.compare_identities([100, 101, 102], earlier, 1, self.PERIOD)['isoPixelArrivalFidelityPassed'])

    def test_hold_count_must_match_the_writer(self):
        decoded = [(100, 0.0), (101, self.PERIOD), (101, 1.1), (101, 2.1), (102, 2.5)]
        for reported in (0, 1, 3):
            with self.subTest(reported=reported):
                result = iso.compare_identities([100, 101, 102], decoded, reported, self.PERIOD)
                self.assertFalse(result['isoPixelArrivalFidelityPassed'])
                self.assertFalse(result['holdsMatched'])
        self.assertTrue(iso.compare_identities([100, 101, 102], decoded, 2, self.PERIOD)['isoPixelArrivalFidelityPassed'])

    def test_untimed_repeats_are_never_excused(self):
        result = iso.compare_identities([100, 101, 102], [100, 101, 101, 102], 1)
        self.assertFalse(result['isoPixelArrivalFidelityPassed'])

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
