import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('pictures', Path(__file__).with_name('source-picture-verdict.py'))
pictures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pictures)


def bands(identity):
    bits = [cell in (0, 35) if cell in (0, 1, 34, 35) else bool(identity & (1 << (cell - 2))) for cell in range(36)]
    return [235 if bit else 16 for bit in bits] + [16 if bit else 235 for bit in bits]


def fixture():
    manifest = dict(startQpc=1_000_000_000, endQpc=1_010_000_000, qpcFrequency=10_000_000, minimumSeconds=1,
                    cameraSourceId='capture:camera', screenSourceId='capture:screen:2',
                    points=[[0, 0]] * 160, cameraMarker=list(range(72)), screenMarker=list(range(72, 144)),
                    cameraBody=list(range(144, 160)), maximumScreenPictureLag=2, maximumCameraObservationAgeMs=100,
                    maximumScreenAgeMs=200, maximumCameraBodyHoldMs=500, cameraRateNumerator=60, cameraRateDenominator=1)
    stimulus = [dict(identity=i + 1, beforeNs=100_000_000_000 + i * 100_000_000,
                     afterNs=100_000_100_000 + i * 100_000_000) for i in range(11)]
    rows, draws = [], {}
    for i in range(60):
        arrival = 100_001_000 + i * 1_000_000 // 60
        marker = i // 6 + 1
        values = bands(i + 100) + bands(marker) + [40 + j * 8 + i % 3 for j in range(16)]
        rows.append(dict(sample=i+1, identity=i+1, arrivalHostUs=arrival, pictureLuma=bytes(values).hex()))
        draws[i+1] = dict(sourceFrameId=i+100, observation100ns=arrival*10-500_000, reason=1)
    return rows, manifest, draws, stimulus


class SourcePictures(unittest.TestCase):
    def test_exact_received_source_pictures_and_independent_screen_stimulus_pass(self):
        self.assertTrue(pictures.judge_samples(*fixture())['sourcePicturesVerified'])

    def test_advancing_output_counter_cannot_hide_an_older_camera_picture(self):
        rows, manifest, draws, stimulus = fixture()
        raw = bytes.fromhex(rows[30]['pictureLuma'])
        rows[30]['pictureLuma'] = (bytes(bands(129)) + raw[72:]).hex()
        verdict = pictures.judge_samples(rows, manifest, draws, stimulus)
        self.assertFalse(verdict['sourcePicturesVerified'])
        self.assertEqual(verdict['mismatched'], 1)

    def test_advancing_camera_and_program_counters_cannot_hide_frozen_screen(self):
        rows, manifest, draws, stimulus = fixture()
        for row in rows:
            raw = bytes.fromhex(row['pictureLuma'])
            row['pictureLuma'] = (raw[:72] + bytes(bands(1)) + raw[144:]).hex()
        verdict = pictures.judge_samples(rows, manifest, draws, stimulus)
        self.assertFalse(verdict['sourcePicturesVerified'])
        self.assertGreater(verdict['stale'], 0)

    def test_markers_cannot_hide_frozen_ordinary_camera_picture(self):
        rows, manifest, draws, stimulus = fixture()
        body = bytes.fromhex(rows[0]['pictureLuma'])[144:]
        for row in rows: row['pictureLuma'] = (bytes.fromhex(row['pictureLuma'])[:144] + body).hex()
        self.assertFalse(pictures.judge_samples(rows, manifest, draws, stimulus)['sourcePicturesVerified'])

    def test_matching_draw_metadata_cannot_waive_capture_selection_repeat_and_skip(self):
        rows, manifest, draws, stimulus = fixture()
        raw = bytes.fromhex(rows[30]['pictureLuma'])
        rows[30]['pictureLuma'] = (bytes(bands(129)) + raw[72:]).hex()
        draws[31]['sourceFrameId'] = 129
        verdict = pictures.judge_samples(rows, manifest, draws, stimulus)
        self.assertEqual(verdict['mismatched'], 0)
        self.assertEqual(verdict['cameraRepeated'], 1)
        self.assertEqual(verdict['cameraMissing'], 1)
        self.assertFalse(verdict['sourcePicturesVerified'])

    def test_torn_missing_and_unattributable_picture_samples_fail(self):
        for mode in ('torn', 'missing', 'unattributed'):
            rows, manifest, draws, stimulus = fixture()
            if mode == 'missing': rows[10].pop('pictureLuma')
            elif mode == 'unattributed': draws.pop(11)
            else:
                raw = bytearray.fromhex(rows[10]['pictureLuma']); raw[36] = raw[0]
                rows[10]['pictureLuma'] = raw.hex()
            self.assertFalse(pictures.judge_samples(rows, manifest, draws, stimulus)['sourcePicturesVerified'])


if __name__ == '__main__': unittest.main()
