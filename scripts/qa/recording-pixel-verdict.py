"""Decode QA Program identities from finalized MP4 pixels, without FPS padding.

This proves recorded pixel continuity only, never display presentation or camera
delivery. Run after qualification so decoding does not compete with the show.
"""
import argparse
import json
import subprocess
import tempfile


def decode_rows(pixels, width=1920):
    if len(pixels) != width * 2:
        return None
    cell_width = (width // 36) & ~1
    identity = 0
    for cell in range(36):
        x = cell * cell_width + cell_width // 2
        top, bottom = pixels[x], pixels[width + x]
        if not ((top < 64 and bottom > 192) or (top > 192 and bottom < 64)):
            return None
        bit = top > 192
        if cell in (0, 1, 34, 35):
            if bit != (cell in (0, 35)):
                return None
        elif bit:
            identity |= 1 << (cell - 2)
    return identity


def judge(path, ffmpeg="ffmpeg", ffprobe="ffprobe", minimum_seconds=1800, first_identity=None, last_identity=None):
    if minimum_seconds <= 0 or (first_identity is None) != (last_identity is None):
        raise ValueError("Use a positive duration and both identity boundaries or neither")
    if first_identity is not None and not (0 <= first_identity <= last_identity <= 0xffffffff):
        raise ValueError("Invalid identity interval")
    probe = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,r_frame_rate", "-of", "json", path],
        capture_output=True, text=True, check=True)
    streams = json.loads(probe.stdout).get("streams", [])
    if len(streams) != 1 or streams[0] != {"width": 1920, "height": 1080, "r_frame_rate": "60/1"}:
        raise ValueError("Expected a finalized 1920x1080 60/1 Program recording")
    # Luma guard rows only; the decoder still observes every real encoded frame.
    filters = ("format=gray,split[t][b];[t]crop=1920:1:0:16[top];"
               "[b]crop=1920:1:0:1064[bottom];[top][bottom]vstack")
    samples = invalid = repeated = missing = reordered = decoded = 0
    first = previous = last = None
    interval_ended = False
    range_violations = 0
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([ffmpeg, "-nostdin", "-v", "error", "-i", path,
            "-filter_complex", filters, "-an", "-fps_mode", "passthrough",
            "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"],
            stdout=subprocess.PIPE, stderr=errors)
        try:
            while True:
                pixels = process.stdout.read(3840)
                if not pixels:
                    break
                decoded += 1
                if decoded > 2_000_000:
                    raise ValueError("Recording exceeds bounded verifier capacity")
                identity = decode_rows(pixels)
                if first_identity is not None:
                    if first is None and (identity is None or identity < first_identity):
                        continue
                    if interval_ended:
                        if identity is not None and identity <= last_identity:
                            range_violations += 1
                        continue
                samples += 1
                if identity is None:
                    invalid += 1
                elif previous is not None:
                    delta = identity - previous
                    repeated += delta == 0
                    reordered += delta < 0
                    missing += max(0, delta - 1)
                if first is None and identity is not None:
                    first = identity
                last = previous = identity
                if first_identity is not None and identity == last_identity:
                    interval_ended = True
            result = process.wait(timeout=30)
            errors.seek(0)
            error_text = errors.read(4096).decode("utf-8", errors="replace")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    range_complete = first_identity is None or (first == first_identity and last == last_identity
        and samples == last_identity - first_identity + 1)
    complete = result == 0 and not error_text and samples >= minimum_seconds * 60 and range_complete
    return {"schema": "recording-pixel-verdict-v1", "path": path,
        "recordedPixelContinuityPassed": complete and not (invalid or repeated or missing or reordered or range_violations),
        "samples": samples, "firstIdentity": first, "lastIdentity": last,
        "decodedFrames": decoded, "requestedFirstIdentity": first_identity,
        "requestedLastIdentity": last_identity, "identityRangeComplete": range_complete,
        "invalid": invalid, "repeated": repeated, "missing": missing, "reordered": reordered,
        "rangeViolations": range_violations,
        "decoderComplete": result == 0, "decoderErrors": error_text,
        "minimumSeconds": minimum_seconds,
        "limitation": "Finalized recording pixels only; does not establish real-time output deadlines, audio or display presentation."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--minimum-seconds", type=int, default=1800)
    parser.add_argument("--first-identity", type=int)
    parser.add_argument("--last-identity", type=int)
    args = parser.parse_args()
    try:
        verdict = judge(args.recording, args.ffmpeg, args.ffprobe, args.minimum_seconds, args.first_identity, args.last_identity)
        print(json.dumps(verdict, indent=2))
        raise SystemExit(0 if verdict["recordedPixelContinuityPassed"] else 1)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"recordedPixelContinuityPassed": False, "error": str(error)}))
        raise SystemExit(2)
