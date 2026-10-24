"""Tests for outputs-drill-judge.py (#443): verdict aggregation from sample judge
outputs, and the offline orchestration against small synthetic evidence that is
decoded by the real recording/ISO/camera/admission judges.

The synthetic videos carry the same 36-cell QA identity bands the core paints
(COREVIDEO_QA_PROGRAM_COUNTER / COREVIDEO_QA_ISO_COUNTER); the ISO trace uses
the judge's own binary layout; the OS camera evidence follows the
camera-pixel-receiver-v1 NDJSON the receiver writes. The end-to-end cases need
ffmpeg, ffprobe and node on PATH (skipped otherwise).
"""
import importlib.util
import json
import pathlib
import shutil
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
ADMISSIONS_PASS = {'schema': 'program-source-admission-capture-v1', 'samples': 1800, 'failedSamples': 0,
                   'missingEvidenceSamples': 0, 'sourceAdmissionsVerified': True}
FLAGS_TRUE = {flag: True for flag in judge.GATING_FLAGS}


class RowsAndAggregation(unittest.TestCase):
    def all_pass_rows(self):
        return [judge.gate_program_admissions(judge.recording_row('Program recording', 'program', PROGRAM_PASS, 'p', True), ADMISSIONS_PASS),
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

    def test_iso_hold_pattern_passes_but_says_the_source_stalled(self):
        verdict = dict(ISO_PASS, heldFrames=3, heldExcused=3, holdsMatched=True,
                       coverage={'maximumSourceArrivalGapMs': 3016.0})
        row = judge.iso_row('zoom:1', verdict, 'p')
        self.assertEqual(row['status'], judge.PASS)
        self.assertEqual(row['held'], 3)
        self.assertIn('held=3, source stalled', row['reason'])
        self.assertIn('3016 ms', row['reason'])

    def test_iso_healthy_row_has_no_held_note(self):
        row = judge.iso_row('zoom:1', dict(ISO_PASS, heldFrames=0, heldExcused=0, holdsMatched=True), 'p')
        self.assertEqual((row['status'], row['held'], row['reason']), (judge.PASS, 0, None))

    def test_iso_hold_count_mismatch_fails_and_says_so(self):
        verdict = dict(ISO_PASS, isoPixelArrivalFidelityPassed=False, heldFrames=0, heldExcused=2, holdsMatched=False)
        row = judge.iso_row('zoom:1', verdict, 'p')
        self.assertEqual(row['status'], judge.FAIL)
        self.assertIn('writer reported 0, file shows 2', row['reason'])

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

    def test_unbounded_clean_recording_is_its_own_status_and_never_pass(self):
        row = judge.recording_row('Program recording', 'program', PROGRAM_PASS, 'p', False)
        self.assertEqual(row['status'], 'UNBOUNDED')
        self.assertIn('cannot PASS', row['reason'])
        rows = self.all_pass_rows()
        rows[0] = judge.gate_program_admissions(row, ADMISSIONS_PASS)
        self.assertEqual(rows[0]['status'], 'UNBOUNDED')
        self.assertEqual(judge.aggregate(rows, {})['overall'], 'NOT JUDGED')

    def test_unbounded_recording_with_a_gap_still_fails(self):
        row = judge.recording_row('Stream', 'stream', dict(PROGRAM_PASS, recordedPixelContinuityPassed=False, missing=2), 'p', False)
        self.assertEqual(row['status'], 'FAIL')

    def test_program_needs_source_admissions(self):
        clean = judge.recording_row('Program recording', 'program', PROGRAM_PASS, 'p', True)
        failed = judge.gate_program_admissions(clean, dict(ADMISSIONS_PASS, sourceAdmissionsVerified=False, failedSamples=3))
        self.assertEqual(failed['status'], 'FAIL')
        self.assertIn('failed=3', failed['reason'])
        self.assertEqual(judge.gate_program_admissions(clean, {'error': 'CalledProcessError'})['status'], 'NOT JUDGED')
        self.assertEqual(judge.gate_program_admissions(clean, None)['status'], 'NOT JUDGED')
        bad = judge.recording_row('Program recording', 'program', dict(PROGRAM_PASS, recordedPixelContinuityPassed=False, missing=1), 'p', True)
        self.assertEqual(judge.gate_program_admissions(bad, ADMISSIONS_PASS)['status'], 'FAIL')

    def test_throughout_flags(self):
        self.assertEqual(judge.throughout_failures(FLAGS_TRUE), [])
        self.assertEqual(judge.throughout_failures(dict(FLAGS_TRUE, captureProgressThroughout=False)), ['captureProgressThroughout'])
        verdict = judge.aggregate(self.all_pass_rows(), {'throughoutFailures': ['remotes1080Throughout']})
        self.assertEqual(verdict['overall'], 'FAIL')
        self.assertIn('remotes1080Throughout', verdict['notes'][0])

    def test_hardware_placement_is_not_a_gating_flag_but_program_encoder_is(self):
        # #935 / owner ruling D8: an ISO on the software encoder is a note; Program on it is a FAIL.
        self.assertNotIn('hardwareEncodersThroughout', judge.GATING_FLAGS)
        self.assertIn('programEncoderHardwareThroughout', judge.GATING_FLAGS)
        self.assertEqual(judge.throughout_failures(dict(FLAGS_TRUE, hardwareEncodersThroughout=False)), [])
        self.assertEqual(judge.throughout_failures(dict(FLAGS_TRUE, programEncoderHardwareThroughout=False)),
                         ['programEncoderHardwareThroughout'])

    def test_required_flags_are_named_explicitly(self):
        self.assertEqual(judge.REQUIRED_FLAGS, ('programEncoderHardwareThroughout', 'remotes1080Throughout',
                                                'captureProgressThroughout', 'programSourcesAdmittedThroughout'))

    def test_software_iso_notes_carry_the_measured_cost_or_say_not_measured(self):
        measurement = {'encoderPlacement': {'zoom:8': {'paths': ['software'], 'changed': False},
                                            'zoom:1': {'paths': ['hardware'], 'changed': False}},
                       'encoderCost': {'samples': 1800, 'logicalProcessors': 28, 'coreCpuPercent': {'mean': 31.2, 'peak': 47.9},
                                       'software': {'zoom:8': {'isoIndex': 8, 'meanVideoWorkUs': 9400, 'peakIntervalVideoWorkUs': 14100,
                                                               'droppedFramesStart': 0, 'droppedFramesEnd': 0, 'frameRate': 60}}}}
        self.assertEqual(judge.software_iso_sources(measurement), ['zoom:8'])
        notes = judge.software_iso_notes(measurement)
        self.assertEqual(notes, ['ISO-8 zoom:8 on software encoder: mean 9.4 ms of 16.7 ms per frame, peak interval 14.1 ms, '
                                 'dropped frames 0 -> 0, core CPU mean 31.2 % / peak 47.9 %'])
        bare = judge.software_iso_notes({'encoderPlacement': {'zoom:8': {'paths': ['software'], 'changed': False}}})
        self.assertEqual(bare, ['ISO zoom:8 on software encoder: mean not measured, peak interval not measured, '
                                'dropped frames not measured, core CPU not measured'])
        changed = judge.software_iso_notes({'encoderPlacement': {'zoom:8': {'paths': ['hardware', 'software'], 'changed': True}}})
        self.assertTrue(any('changed encoder during the run' in n for n in changed), changed)
        self.assertEqual(judge.software_iso_notes({}), [])

    def test_iso_row_carries_encoder_but_status_comes_only_from_identity(self):
        row = judge.with_encoder(judge.iso_row('zoom:8', ISO_PASS, 'p'), 'software')
        self.assertEqual((row['status'], row['encoder']), ('PASS', 'software'))
        bad = judge.with_encoder(judge.iso_row('zoom:8', dict(ISO_PASS, isoPixelArrivalFidelityPassed=False, missing=1), 'p'), 'software')
        self.assertEqual(bad['status'], 'FAIL')
        text = judge.summary_text(judge.aggregate([row], {'seconds': 60, 'workload': 'w'}))
        self.assertRegex(text, r'ISO zoom:8\s+PASS.* encoder=software')

    def test_verdict_schema_is_v3(self):
        self.assertEqual(judge.aggregate([], {})['schema'], 'outputs-drill-verdict-v3')

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
        self.assertEqual(judge.camera_row(CAMERA_PASS, 'p', True, False)['status'], 'NOT JUDGED')

    def test_camera_not_covering_interval_fails(self):
        self.assertEqual(judge.camera_row(CAMERA_PASS, 'p', False, True)['status'], 'FAIL')

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


# ---------------------------------------------------------------- synthetic evidence

FREQUENCY, START = 10_000_000, 50_000_000
END = START + FREQUENCY + FREQUENCY // 60 * 2
WEBCAM = 'capture:f4978a7f6d614c0d'
PROGRAM_SOURCES = [WEBCAM, 'capture:screen:0']


def paint(identity, width=1920, height=1080):
    """One yuv420p frame carrying the QA identity bands decode_rows reads."""
    luma = bytearray([128]) * (width * height)
    cell = (width // 36) & ~1
    for index in range(36):
        bit = True if index in (0, 35) else False if index in (1, 34) else bool(identity >> (index - 2) & 1)
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


def write_trace(path, source, frames):
    epoch = 777
    tag = iso.source_tag(source, epoch)
    events = [iso.EVENT.pack(epoch, tag, 5, -1, frame, START + 100 + i * (FREQUENCY // 60), 1 + i,
                             1920 << 32 | 1080, 11, 0) for i, frame in enumerate(frames)]
    header = iso.HEADER.pack(b'CVTRACE1', 1, iso.EVENT.size, FREQUENCY, epoch, len(events), 0, 0, 1,
                             START - 1000, START + 3 * FREQUENCY)
    path.write_bytes(header + b''.join(events))


def write_camera(directory, drop=None):
    """camera-pixel-receiver-v1 NDJSON: one sample per 16.667 ms, identity 99
    just before the interval, 100.. inside it, brackets past the end."""
    directory.mkdir()
    start_us = START * 1_000_000 // FREQUENCY
    lines = [json.dumps({'schema': 'camera-pixel-receiver-v1', 'receiverMode': 'os-camera', 'width': 1920, 'height': 1080,
                         'fpsNumerator': 60, 'fpsDenominator': 1, 'startUnixUs': 1_700_000_000_000_000})]
    count = 0
    for i in range(66):
        if drop is not None and 99 + i == drop:
            continue
        count += 1
        lines.append(json.dumps({'sample': count, 'arrivalUs': 1_000 + i * 16_667, 'pts100ns': 10 + i * 166_667,
                                 'arrivalHostUs': start_us - 10_000 + i * 16_667, 'identity': 99 + i}))
    lines.append(json.dumps({'complete': True, 'samples': count, 'frameFailures': 0}))
    (directory / 'receiver.ndjson').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (directory / 'result.json').write_text(json.dumps({'exitCode': 0}), encoding='utf-8')


def admission(source_id, ok=True):
    return {'sourceId': source_id, 'state': 'ready' if ok else 'stale', 'actualEpoch': 1, 'requestedEpoch': 1,
            'actualFrameId': 9, 'requestedFrameId': 9, 'actualCapture100ns': 5, 'requestedCapture100ns': 5}


def write_samples(path, admissions_ok=True):
    rows = []
    for i, qpc in enumerate(range(START, END + 1, FREQUENCY // 2)):
        rows.append(json.dumps({'utc': f'2026-10-23T00:00:0{i}Z', 'qpc': str(qpc), 'snapshot': {
            'programFrame': {'health': 'live', 'videoSources': [{'sourceId': s} for s in PROGRAM_SOURCES]},
            'programSourceAdmission': {'version': 1, 'sources': [admission(PROGRAM_SOURCES[0], admissions_ok or i != 1),
                                                                 admission(PROGRAM_SOURCES[1])]}}}))
    path.write_text('\n'.join(rows) + '\n', encoding='utf-8')


@unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe') and shutil.which('node'), 'ffmpeg/ffprobe/node not on PATH')
class EndToEndOffline(unittest.TestCase):
    """run() over synthetic evidence through the real judges."""

    def evidence(self, root, identities, camera=True, flags=None, admissions_ok=True, camera_drop=None, extra_measurement=None):
        recordings = root / 'recordings'
        recordings.mkdir()
        encode(recordings / 'Program.mp4', identities)
        encode(recordings / 'ISO-01.mp4', identities)
        encode(root / 'stream-received.ts', identities, ('-f', 'mpegts'))
        write_trace(root / 'arrival-trace.bin', WEBCAM, list(range(100, 160)))
        write_samples(root / 'samples.ndjson', admissions_ok)
        if camera:
            write_camera(root / 'os-camera', camera_drop)
            (root / 'camera-serving-identity.json').write_text('{}', encoding='utf-8')
        (root / 'drill.json').write_text(json.dumps({
            'seconds': 1, 'workload': 'synthetic', 'programSourceIds': PROGRAM_SOURCES,
            'outputs': {'program': True, 'iso': [WEBCAM], 'stream': True, 'camera': camera},
            'tools': {'ffmpeg': 'ffmpeg', 'ffprobe': 'ffprobe', 'node': shutil.which('node')}, 'notes': []}), encoding='utf-8')
        (root / 'measurement.json').write_text(json.dumps({
            'start': {'qpc': str(START)}, 'end': {'qpc': str(END)}, 'qpcFrequency': FREQUENCY, 'minimumSeconds': 1,
            **(flags if flags is not None else FLAGS_TRUE), **(extra_measurement or {})}), encoding='utf-8')
        (root / 'finalized.json').write_text(json.dumps({
            'recording': {'lifecycle': {'finalized': True}, 'artifactPath': str(recordings / 'Program.mp4'),
                          'streams': [{'kind': 'program'}, {'kind': 'iso', 'sourceId': WEBCAM, 'path': str(recordings / 'ISO-01.mp4')}]},
            'deliveryEvidence': {'captureComplete': True}}), encoding='utf-8')

    def run_drill(self, identities=range(100, 166), **kwargs):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.evidence(root, identities, **kwargs)
            verdict = judge.run(root)
            return verdict, {row['output']: row for row in verdict['outputs']}

    def test_clean_bounded_run_passes_every_output(self):
        verdict, rows = self.run_drill()
        self.assertEqual({k: v['status'] for k, v in rows.items()},
                         {'Program recording': 'PASS', 'ISO ' + WEBCAM: 'PASS', 'OS virtual camera': 'PASS', 'Stream': 'PASS'}, verdict)
        self.assertEqual(verdict['overall'], 'PASS')
        program = rows['Program recording']
        self.assertEqual((program['expected'], program['decoded'], program['missing'], program['repeated']), (62, 62, 0, 0))
        iso_row = rows['ISO ' + WEBCAM]
        self.assertEqual((iso_row['expected'], iso_row['decoded'], iso_row['missing'], iso_row['repeated']), (60, 60, 0, 0))
        self.assertNotEqual(verdict['supplementary'].get('programGpuDeadlines'), 'PASS')  # synthetic V1 trace proves no deadlines

    def test_one_dropped_frame_fails_program_iso_and_stream(self):
        verdict, rows = self.run_drill([i for i in range(100, 166) if i != 130])
        self.assertEqual(verdict['overall'], 'FAIL')
        for name in ('Program recording', 'ISO ' + WEBCAM, 'Stream'):
            self.assertEqual(rows[name]['status'], 'FAIL', rows[name])
            self.assertEqual(rows[name]['missing'], 1, rows[name])
        self.assertEqual(rows['OS virtual camera']['status'], 'PASS')

    def test_camera_gap_fails_the_camera(self):
        verdict, rows = self.run_drill(camera_drop=120)
        self.assertEqual(rows['OS virtual camera']['status'], 'FAIL')
        self.assertEqual(rows['OS virtual camera']['missing'], 1)
        self.assertEqual(verdict['overall'], 'FAIL')

    def test_camera_skipped_means_unbounded_never_pass(self):
        verdict, rows = self.run_drill(camera=False)
        self.assertEqual(rows['Program recording']['status'], 'UNBOUNDED')
        self.assertEqual(rows['Stream']['status'], 'UNBOUNDED')
        self.assertEqual(rows['ISO ' + WEBCAM]['status'], 'PASS')
        self.assertEqual(verdict['skipped'], ['OS virtual camera'])
        self.assertEqual(verdict['overall'], 'NOT JUDGED')

    def test_false_throughout_flag_fails_a_clean_run(self):
        verdict, _ = self.run_drill(flags=dict(FLAGS_TRUE, captureProgressThroughout=False))
        self.assertEqual(verdict['overall'], 'FAIL')
        self.assertEqual(verdict['throughoutFailures'], ['captureProgressThroughout'])

    def test_missing_throughout_flags_fail(self):
        verdict, _ = self.run_drill(flags={})
        self.assertEqual(verdict['overall'], 'FAIL')
        self.assertIn('remotes1080Throughout (not recorded)', verdict['throughoutFailures'])
        self.assertIn('programEncoderHardwareThroughout (not recorded)', verdict['throughoutFailures'])
        self.assertNotIn('hardwareEncodersThroughout (not recorded)', verdict['throughoutFailures'])

    def test_software_iso_placement_alone_does_not_fail_a_clean_run(self):
        verdict, rows = self.run_drill(flags=dict(FLAGS_TRUE, hardwareEncodersThroughout=False),
                                       extra_measurement={'encoderCost': {'software': {WEBCAM: {'isoIndex': 1, 'meanVideoWorkUs': 9400}}}})
        self.assertEqual(verdict['overall'], 'PASS', verdict)
        self.assertEqual(verdict['throughoutFailures'], [])
        self.assertEqual(rows['ISO ' + WEBCAM]['encoder'], 'software')
        self.assertTrue(any('on software encoder' in n for n in verdict['notes']), verdict['notes'])
        self.assertEqual(verdict['schema'], 'outputs-drill-verdict-v3')

    def test_software_iso_with_missing_identity_still_fails(self):
        verdict, rows = self.run_drill([i for i in range(100, 166) if i != 130],
                                       flags=dict(FLAGS_TRUE, hardwareEncodersThroughout=False),
                                       extra_measurement={'encoderPlacement': {WEBCAM: {'paths': ['software'], 'changed': False}}})
        self.assertEqual(rows['ISO ' + WEBCAM]['status'], 'FAIL')
        self.assertEqual(rows['ISO ' + WEBCAM]['encoder'], 'software')
        self.assertEqual(verdict['overall'], 'FAIL')

    def test_program_on_software_encoder_fails_a_clean_run(self):
        # Owner ruling D8: Program on the software encoder stays a FAIL for RC.
        verdict, _ = self.run_drill(flags=dict(FLAGS_TRUE, hardwareEncodersThroughout=False, programEncoderHardwareThroughout=False))
        self.assertEqual(verdict['overall'], 'FAIL')
        self.assertEqual(verdict['throughoutFailures'], ['programEncoderHardwareThroughout'])

    def test_unadmitted_program_source_fails_program(self):
        verdict, rows = self.run_drill(admissions_ok=False)
        self.assertEqual(rows['Program recording']['status'], 'FAIL')
        self.assertIn('admissions not verified', rows['Program recording']['reason'])
        self.assertEqual(verdict['overall'], 'FAIL')

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
