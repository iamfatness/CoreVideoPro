"""Tests for outputs-drill-judge.py (#443): verdict aggregation from sample judge
outputs, and the offline orchestration against small synthetic evidence that is
decoded by the real recording/ISO judges through ffmpeg.

The synthetic videos carry the same 36-cell QA identity bands the core paints
(COREVIDEO_QA_PROGRAM_COUNTER / COREVIDEO_QA_ISO_COUNTER); the ISO trace uses
the judge's own binary layout. Requires ffmpeg and ffprobe on PATH for the
end-to-end cases (skipped otherwise).
"""
import importlib.util
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('outputs_drill_judge', HERE / 'outputs-drill-judge.py')
judge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(judge)
iso_spec = importlib.util.spec_from_file_location('iso_pixels_for_drill', HERE / 'iso-pixel-verdict.py')
iso = importlib.util.module_from_spec(iso_spec)
iso_spec.loader.exec_module(iso)

# Shapes copied from the 2026-10-22 soak's offline-bounded verdicts.
PROGRAM_PASS = {'schema': 'recording-pixel-verdict-v1', 'recordedPixelContinuityPassed': True, 'samples': 108036,
                'firstIdentity': 13238, 'lastIdentity': 121273, 'requestedFirstIdentity': 13238,
                'requestedLastIdentity': 121273, 'identityRangeComplete': True, 'invalid': 0, 'repeated': 0,
                'missing': 0, 'reordered': 0, 'rangeViolations': 0, 'decoderComplete': True, 'decoderErrors': '',
                'minimumSeconds': 1800}
ISO_PASS = {'isoPixelArrivalFidelityPassed': True, 'expectedSamples': 53891, 'recordedSamples': 53891,
            'missing': 0, 'repeated': 0, 'firstIdentity': 11250, 'lastIdentity': 119030, 'invalid': 0,
            'rangeViolations': 0, 'firstMismatchIndex': None, 'decoderComplete': True, 'decoderErrors': ''}
CAMERA_PASS = {'schema': 'camera-pixel-verdict-v1', 'osCameraContinuityVerified': True, 'samples': 108038,
               'firstIdentity': 13237, 'lastIdentity': 121274, 'invalid': 0, 'repeated': 0, 'missing': 0,
               'reordered': 0, 'errors': [], 'coverage': {'complete': True}}


class RowsAndAggregation(unittest.TestCase):
    def all_pass_rows(self):
        return [judge.recording_row('Program recording', 'program', PROGRAM_PASS, 'judges/program.json', True),
                judge.iso_row('zoom:1', ISO_PASS, 'judges/iso.json'),
                judge.camera_row(CAMERA_PASS, 'judges/os-camera.json', True, True),
                judge.recording_row('Stream', 'stream', PROGRAM_PASS, 'judges/stream.json', True)]

    def test_every_output_pass_is_overall_pass_with_counts(self):
        rows = self.all_pass_rows()
        verdict = judge.aggregate(rows, {'seconds': 1800, 'workload': 'w'})
        self.assertEqual(verdict['overall'], 'PASS')
        self.assertEqual([r['status'] for r in rows], ['PASS'] * 4)
        self.assertEqual((rows[0]['expected'], rows[0]['decoded'], rows[0]['missing']), (108036, 108036, 0))
        self.assertEqual((rows[1]['expected'], rows[1]['decoded']), (53891, 53891))
        self.assertEqual(rows[2]['expected'], 121274 - 13237 + 1)

    def test_one_missing_frame_fails_overall(self):
        missing = dict(PROGRAM_PASS, recordedPixelContinuityPassed=False, missing=1, samples=108035)
        rows = self.all_pass_rows()
        rows[3] = judge.recording_row('Stream', 'stream', missing, 'judges/stream.json', True)
        verdict = judge.aggregate(rows, {})
        self.assertEqual(rows[3]['status'], 'FAIL')
        self.assertIn('missing=1', rows[3]['reason'])
        self.assertEqual(verdict['overall'], 'FAIL')

    def test_iso_repeat_fails(self):
        row = judge.iso_row('zoom:1', dict(ISO_PASS, isoPixelArrivalFidelityPassed=False, repeated=2, firstMismatchIndex=7), 'p')
        self.assertEqual(row['status'], 'FAIL')
        self.assertIn('repeated=2', row['reason'])

    def test_judge_error_is_not_judged_and_never_pass(self):
        rows = self.all_pass_rows()
        rows[1] = judge.iso_row('zoom:1', {'error': 'ValueError: No source arrivals'}, 'p')
        self.assertEqual(rows[1]['status'], 'NOT JUDGED')
        self.assertIn('No source arrivals', rows[1]['reason'])
        self.assertEqual(judge.aggregate(rows, {})['overall'], 'NOT JUDGED')

    def test_fail_outranks_not_judged(self):
        rows = [judge.not_judged('ISO a', 'iso', 'x'),
                judge.recording_row('Program recording', 'program', dict(PROGRAM_PASS, recordedPixelContinuityPassed=False, repeated=3), 'p', True)]
        self.assertEqual(judge.aggregate(rows, {})['overall'], 'FAIL')

    def test_no_outputs_is_not_pass(self):
        self.assertEqual(judge.aggregate([], {})['overall'], 'NOT JUDGED')

    def test_collection_failure_is_fail_even_if_rows_pass(self):
        verdict = judge.aggregate(self.all_pass_rows(), {'collectionFailure': 'Native core restarted'})
        self.assertEqual(verdict['overall'], 'FAIL')
        self.assertTrue(verdict['notes'][0].startswith('Live collection aborted: Native core restarted'))

    def test_camera_unproven_serving_module_is_not_judged(self):
        row = judge.camera_row(CAMERA_PASS, 'p', True, False)
        self.assertEqual(row['status'], 'NOT JUDGED')

    def test_camera_not_covering_interval_fails(self):
        row = judge.camera_row(CAMERA_PASS, 'p', False, True)
        self.assertEqual(row['status'], 'FAIL')

    def test_unbounded_program_pass_is_labelled(self):
        row = judge.recording_row('Program recording', 'program', PROGRAM_PASS, 'p', False)
        self.assertEqual(row['status'], 'PASS')
        self.assertIn('whole-file', row['reason'])

    def test_summary_has_one_line_per_output_and_skips(self):
        verdict = judge.aggregate(self.all_pass_rows()[:2], {'seconds': 60, 'workload': 'w', 'skipped': ['Stream'],
                                                            'supplementary': {'programGpuDeadlines': 'PASS'}})
        text = judge.summary_text(verdict)
        self.assertTrue(text.startswith('Outputs drill: PASS'))
        self.assertRegex(text, r'Program recording\s+PASS\s+expected=108036 decoded=108036 missing=0 repeated=0')
        self.assertRegex(text, r'ISO zoom:1\s+PASS')
        self.assertRegex(text, r'Stream\s+SKIPPED')
        self.assertIn('supplementary; not part of the overall verdict', text)

    def test_bounded_camera_identities(self):
        rows = [{'sample': i + 1, 'arrivalHostUs': 1000 + i * 10, 'identity': 50 + i} for i in range(10)]
        self.assertEqual(judge.bounded_camera_identities(rows, 1020, 1060), (52, 56, 5))
        self.assertIsNone(judge.bounded_camera_identities(rows, 5000, 6000))


def paint(identity, width=1920, height=1080):
    """One yuv420p frame carrying the QA identity bands decode_rows reads."""
    luma = bytearray([128]) * (width * height)
    cell = (width // 36) & ~1
    for index in range(36):
        if index in (0, 35):
            bit = True
        elif index in (1, 34):
            bit = False
        else:
            bit = bool(identity >> (index - 2) & 1)
        top, bottom = (235, 16) if bit else (16, 235)
        for y in range(0, 40):
            luma[y * width + index * cell:y * width + (index + 1) * cell] = bytes([top]) * cell
        for y in range(height - 40, height):
            luma[y * width + index * cell:y * width + (index + 1) * cell] = bytes([bottom]) * cell
    return bytes(luma) + bytes([128]) * (width * height // 2)


def encode(path, identities, container_args=()):
    process = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-s', '1920x1080',
                                '-r', '60', '-i', 'pipe:0', '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '12',
                                '-pix_fmt', 'yuv420p', *container_args, str(path)], stdin=subprocess.PIPE)
    for identity in identities:
        process.stdin.write(paint(identity))
    process.stdin.close()
    if process.wait(timeout=120):
        raise RuntimeError('ffmpeg encode failed')


def write_trace(path, source, frames, start, frequency=10_000_000):
    epoch = 777
    tag = iso.source_tag(source, epoch)
    events = [iso.EVENT.pack(epoch, tag, 5, -1, frame, start + 100 + i * (frequency // 60), 1 + i,
                             1920 << 32 | 1080, 11, 0) for i, frame in enumerate(frames)]
    began, ended = start - 1000, start + 3 * frequency
    header = iso.HEADER.pack(b'CVTRACE1', 1, iso.EVENT.size, frequency, epoch, len(events), 0, 0, 1, began, ended)
    path.write_bytes(header + b''.join(events))


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'ffmpeg/ffprobe not on PATH')
class EndToEndOffline(unittest.TestCase):
    """run() over synthetic evidence: Program + webcam ISO + stream, camera skipped."""

    def evidence(self, root, identities):
        frequency, start = 10_000_000, 50_000_000
        source = 'capture:f4978a7f6d614c0d'
        recordings = root / 'recordings'
        recordings.mkdir()
        encode(recordings / 'Program.mp4', identities)
        encode(recordings / 'ISO-01.mp4', identities)
        encode(root / 'stream-received.ts', identities, ('-f', 'mpegts'))
        write_trace(root / 'arrival-trace.bin', source, list(range(100, 160)), start, frequency)
        (root / 'drill.json').write_text(json.dumps({
            'seconds': 1, 'workload': 'synthetic', 'programSourceIds': [source, 'capture:screen:0'],
            'outputs': {'program': True, 'iso': [source], 'stream': True, 'camera': False},
            'tools': {'ffmpeg': 'ffmpeg', 'ffprobe': 'ffprobe', 'node': 'node'}, 'notes': []}), encoding='utf-8')
        (root / 'measurement.json').write_text(json.dumps({
            'start': {'qpc': str(start)}, 'end': {'qpc': str(start + frequency + frequency // 60 * 2)},
            'qpcFrequency': frequency, 'minimumSeconds': 1}), encoding='utf-8')
        (root / 'finalized.json').write_text(json.dumps({
            'recording': {'lifecycle': {'finalized': True}, 'artifactPath': str(recordings / 'Program.mp4'),
                          'streams': [{'kind': 'program'}, {'kind': 'iso', 'sourceId': source, 'path': str(recordings / 'ISO-01.mp4')}]},
            'deliveryEvidence': {'captureComplete': True}}), encoding='utf-8')
        return source

    def test_clean_run_passes_every_enabled_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = self.evidence(root, range(100, 160))
            verdict = judge.run(root)
            statuses = {row['output']: row['status'] for row in verdict['outputs']}
            self.assertEqual(statuses, {'Program recording': 'PASS', 'ISO ' + source: 'PASS', 'Stream': 'PASS'}, verdict)
            self.assertEqual(verdict['overall'], 'PASS')
            self.assertEqual(verdict['skipped'], ['OS virtual camera'])
            iso_row = next(row for row in verdict['outputs'] if row['kind'] == 'iso')
            self.assertEqual((iso_row['expected'], iso_row['decoded'], iso_row['missing'], iso_row['repeated']), (60, 60, 0, 0))
            self.assertNotIn(verdict['supplementary'].get('programGpuDeadlines'), ('PASS',))  # synthetic V1 trace proves no deadlines

    def test_one_dropped_frame_fails_program_iso_and_stream(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = self.evidence(root, [i for i in range(100, 160) if i != 130])
            verdict = judge.run(root)
            rows = {row['output']: row for row in verdict['outputs']}
            self.assertEqual(verdict['overall'], 'FAIL')
            for name in ('Program recording', 'ISO ' + source, 'Stream'):
                self.assertEqual(rows[name]['status'], 'FAIL', rows[name])
                self.assertEqual(rows[name]['missing'], 1, rows[name])

    def test_collection_failure_file_marks_everything_not_judged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / 'drill.json').write_text(json.dumps({
                'seconds': 60, 'workload': 'w', 'outputs': {'program': True, 'iso': ['zoom:1'], 'stream': True, 'camera': True},
                'tools': {}, 'notes': []}), encoding='utf-8')
            (root / 'collection-failure.json').write_text(json.dumps({'error': 'Native core restarted'}), encoding='utf-8')
            verdict = judge.run(root)
            self.assertEqual(verdict['overall'], 'FAIL')
            self.assertEqual([row['status'] for row in verdict['outputs']], ['NOT JUDGED'] * 4)


if __name__ == '__main__':
    unittest.main()
