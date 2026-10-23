import csv
import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('display_verdict', pathlib.Path(__file__).with_name('display-timing-verdict.py'))
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class DisplayTimingTests(unittest.TestCase):
    def setUp(self):
        self.manifest = dict(startQpc=1000, endQpc=10001000, qpcFrequency=10000000, minimumSeconds=1,
                             processId=123, refreshNumerator=60000, refreshDenominator=1000,
                             surfaces={'multiview': dict(swapChainAddress='0xABC', chainRevision=2)})
        self.observations = [dict(label='multiview', backend='native-independent', swapChainAddress='0xABC',
                                  chainRevision=2, observedQpc=t) for t in [999, 10001001]]
        self.rows = [dict(ProcessID=123, SwapChainAddress='0xABC', TimeInQPC=1000 + round(n * 10000000 / 60),
                          MsUntilDisplayed='0.01') for n in range(61)]

    def judge(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'display.csv'
            with path.open('w', newline='') as file:
                writer = csv.DictWriter(file, fieldnames=list(self.rows[0])); writer.writeheader(); writer.writerows(self.rows)
            return module.judge(path, self.manifest, self.observations)

    def test_mapped_physical_timing_passes(self):
        self.assertEqual(self.judge()['result'], 'PASS')

    def test_dropped_frame_fails_even_with_near_60_average(self):
        self.rows[30]['MsUntilDisplayed'] = 'NA'
        self.assertEqual(self.judge()['result'], 'FAIL')

    def test_another_chain_or_process_cannot_supply_video_proof(self):
        for field, value in [('ProcessID', 999), ('SwapChainAddress', '0xDEF')]:
            old = [row.copy() for row in self.rows]
            for row in self.rows: row[field] = value
            self.assertEqual(self.judge()['result'], 'FAIL')
            self.rows = old

    def test_missing_or_changed_native_lease_fails(self):
        self.observations.pop()
        self.assertEqual(self.judge()['result'], 'FAIL')

    def test_continuous_counts_cannot_hide_a_long_gap_or_duplicate_display(self):
        self.rows[30]['MsUntilDisplayed'] = '20'
        self.assertEqual(self.judge()['result'], 'FAIL')
        self.setUp(); self.rows[30]['TimeInQPC'] = self.rows[29]['TimeInQPC']
        self.assertEqual(self.judge()['result'], 'FAIL')
        self.setUp(); self.observations[1]['chainRevision'] = 3
        self.assertEqual(self.judge()['result'], 'FAIL')

    def test_wrong_refresh_or_missing_inventory_is_invalid(self):
        self.manifest['refreshNumerator'] = 59951
        with self.assertRaises(ValueError): self.judge()
        self.setUp(); self.manifest['surfaces'] = {}
        with self.assertRaises(ValueError): self.judge()


if __name__ == '__main__': unittest.main()
