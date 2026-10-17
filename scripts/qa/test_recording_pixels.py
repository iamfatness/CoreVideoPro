import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch, Mock
import io

spec = importlib.util.spec_from_file_location("recording_pixels", Path(__file__).with_name("recording-pixel-verdict.py"))
pixels = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pixels)


class RecordingPixelDecoderTests(unittest.TestCase):
    def pattern(self, payload):
        result = bytearray(3840)
        for i, bit in enumerate("10" + payload + "01"):
            x = i * 52 + 26
            result[x] = 220 if bit == "1" else 24
            result[1920 + x] = 24 if bit == "1" else 220
        return result

    def test_known_little_endian_identity_and_compressed_luma(self):
        # Payload is the low bit of each byte, in least-significant-bit order.
        self.assertEqual(pixels.decode_rows(self.pattern("10000000" * 4)), 0x01010101)
        self.assertEqual(pixels.decode_rows(self.pattern("0" * 32)), 0)
        self.assertEqual(pixels.decode_rows(self.pattern("1" * 32)), 0xffffffff)

    def test_torn_guard_or_payload_is_rejected(self):
        for cell in (0, 1, 9, 34, 35):
            sample = self.pattern("10000000" * 4)
            x = cell * 52 + 26
            sample[1920 + x] = sample[x]
            self.assertIsNone(pixels.decode_rows(sample))

    def test_partial_or_ambiguous_frame_is_rejected(self):
        self.assertIsNone(pixels.decode_rows(bytes(3839)))
        self.assertIsNone(pixels.decode_rows(bytes([128]) * 3840))

    def test_invalid_interval_cannot_be_qualified(self):
        for first, last in ((1, None), (None, 1), (-1, 20), (20, 19)):
            with self.assertRaises(ValueError):
                pixels.judge("unused.mp4", first_identity=first, last_identity=last)

    def verdict(self, identities):
        data = b"".join(self.pattern("".join(str((n >> bit) & 1) for bit in range(32)))
            if n is not None else bytes([128]) * 3840 for n in identities)
        decoder = Mock(stdout=io.BytesIO(data))
        decoder.wait.return_value = 0
        decoder.poll.return_value = 0
        probe = Mock(stdout='{"streams":[{"width":1920,"height":1080,"r_frame_rate":"60/1"}]}')
        with patch.object(pixels.subprocess, "run", return_value=probe), \
             patch.object(pixels.subprocess, "Popen", return_value=decoder):
            return pixels.judge("test.mp4", minimum_seconds=1, first_identity=100, last_identity=159)

    def test_actual_bounded_interval_passes_with_prefix_and_suffix(self):
        self.assertTrue(self.verdict(range(95, 165))["recordedPixelContinuityPassed"])

    def test_interval_cannot_hide_out_of_range_or_repeated_pixels(self):
        for bad in (99, 160, 120, None):
            frames = list(range(100, 160))
            frames.insert(30, bad)
            self.assertFalse(self.verdict(frames)["recordedPixelContinuityPassed"])
        self.assertFalse(self.verdict(list(range(100, 160)) + [159])["recordedPixelContinuityPassed"])

    def test_missing_boundary_or_middle_fails(self):
        for removed in (100, 130, 159):
            self.assertFalse(self.verdict([n for n in range(100, 160) if n != removed])["recordedPixelContinuityPassed"])


if __name__ == "__main__":
    unittest.main()
