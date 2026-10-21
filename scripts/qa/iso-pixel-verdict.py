"""Match finalized ISO pixel identities to offered arrivals, at source cadence.

Requires explicit QA ISO counter bands and a completed program-and-iso trace.
Run decoding after the live interval. This is not physical display evidence.
"""
import argparse
import importlib.util
import json
import pathlib
import struct
import subprocess
import tempfile
from fractions import Fraction

HEADER = struct.Struct('<8sIIQQQQQQqq')
EVENT = struct.Struct('<QQQqqqqQII')


def source_tag(source_id, epoch):
    value = 14695981039346656037 ^ epoch
    for byte in ('iso:' + source_id).encode('utf-8'):
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def expected_arrivals(path, source_id, start, end, minimum_seconds):
    if minimum_seconds <= 0 or start >= end:
        raise ValueError('Positive measured interval and duration are required')
    with open(path, 'rb') as stream:
        raw = stream.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError('Partial trace header')
        magic, version, size, frequency, epoch, count, lost, failures, complete, began, ended = HEADER.unpack(raw)
        if magic != b'CVTRACE1' or version != 1 or size != EVENT.size or not frequency or not epoch:
            raise ValueError('Unsupported trace format/clock')
        if complete != 1 or lost or failures or not began <= start < end <= ended:
            raise ValueError('Trace incomplete, lossy or outside measured interval')
        if pathlib.Path(path).stat().st_size != HEADER.size + count * EVENT.size or count * EVENT.size > 256 * 1024 * 1024:
            raise ValueError('Truncated or oversized trace')
        if (end - start) / frequency < minimum_seconds:
            raise ValueError('Source interval is too short')
        tag = source_tag(source_id, epoch)
        found = []
        epochs = set()
        for _ in range(count):
            event = EVENT.unpack(stream.read(EVENT.size))
            event_epoch, source, generation, _, frame, at, observed, layout, stage, reason = event
            if event_epoch != epoch or not 1 <= stage <= 11 or not 0 <= reason <= 4 or not began <= at <= ended:
                raise ValueError('Malformed trace identity, clock or stage')
            if stage != 11 or source != tag or not start <= at <= end:
                continue
            if not generation or not 0 <= frame <= 0xffffffff or not observed or layout != (1920 << 32 | 1080):
                raise ValueError('Source lacks stable 1080p identity/observation evidence')
            epochs.add(generation)
            found.append((at, frame))
    if len(epochs) != 1 or not found:
        raise ValueError('No source arrivals or source owner changed during measurement')
    if any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(found, found[1:])):
        raise ValueError('Source identities or observation clock are not strictly advancing')
    return [row[1] for row in found], {
        'durationSeconds': (end - start) / frequency, 'sourceEpoch': next(iter(epochs)),
        'sourceArrivalFps': len(found) * frequency / (end - start),
        'maximumSourceArrivalGapMs': max((b[0] - a[0]) * 1000 / frequency for a, b in zip(found, found[1:])) if len(found) > 1 else None,
    }


def compare_identities(expected, decoded):
    if not expected or any(b <= a for a, b in zip(expected, expected[1:])):
        raise ValueError('Ordered source identities required')
    first, last = expected[0], expected[-1]
    actual = []
    started = ended = False
    range_violations = 0
    for identity in decoded:
        if not started:
            if identity is None or identity < first:
                continue
            started = True
        if ended:
            if identity is not None and identity <= last:
                range_violations += 1
            continue
        if identity is not None and identity > last:
            ended = True
            continue
        actual.append(identity)
    return {'isoPixelArrivalFidelityPassed': actual == expected and range_violations == 0,
            'expectedSamples': len(expected), 'recordedSamples': len(actual),
            'firstIdentity': first, 'lastIdentity': last, 'invalid': actual.count(None),
            'rangeViolations': range_violations,
            'firstMismatchIndex': next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                      min(len(actual), len(expected)) if len(actual) != len(expected) else None)}


def judge(recording, trace, source_id, start, end, minimum_seconds=1800, ffmpeg='ffmpeg', ffprobe='ffprobe'):
    expected, coverage = expected_arrivals(trace, source_id, start, end, minimum_seconds)
    probe = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                            'stream=width,height,time_base', '-of', 'json', recording], capture_output=True, text=True, check=True)
    streams = json.loads(probe.stdout).get('streams', [])
    if len(streams) != 1 or streams[0].get('width') != 1920 or streams[0].get('height') != 1080:
        raise ValueError('Expected finalized 1920x1080 ISO file')
    time_base = str(Fraction(streams[0].get('time_base', '0')))
    if Fraction(time_base) <= 0:
        raise ValueError('Missing ISO time base')
    spec = importlib.util.spec_from_file_location('recording_pixels', pathlib.Path(__file__).with_name('recording-pixel-verdict.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    filters = 'format=gray,split[t][b];[t]crop=1920:1:0:16[top];[b]crop=1920:1:0:1064[bottom];[top][bottom]vstack'
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([ffmpeg, '-nostdin', '-v', 'error', '-i', recording, '-filter_complex', filters,
                                    '-an', '-fps_mode', 'passthrough', '-enc_time_base', time_base,
                                    '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'],
                                   stdout=subprocess.PIPE, stderr=errors)
        def identities():
            for index in range(2_000_001):
                pixels = process.stdout.read(3840)
                if not pixels:
                    return
                if index == 2_000_000:
                    raise ValueError('ISO exceeds bounded decoder capacity')
                yield module.decode_rows(pixels)
        try:
            verdict = compare_identities(expected, identities())
            result = process.wait(timeout=30)
            errors.seek(0)
            text = errors.read(4096).decode('utf8', errors='replace')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    verdict.update(schema='iso-pixel-arrival-verdict-v1', sourceId=source_id, path=recording,
                   trace=trace, startTicks=str(start), endTicks=str(end), coverage=coverage,
                   decoderComplete=result == 0, decoderErrors=text,
                   limitation='Source-arrival fidelity only; source cadence, GPU/display and output deadlines are separate evidence.')
    verdict['isoPixelArrivalFidelityPassed'] &= result == 0 and not text
    return verdict


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording'); parser.add_argument('trace'); parser.add_argument('source_id')
    parser.add_argument('--start-ticks', type=int, required=True); parser.add_argument('--end-ticks', type=int, required=True)
    parser.add_argument('--minimum-seconds', type=int, default=1800)
    parser.add_argument('--ffmpeg', default='ffmpeg'); parser.add_argument('--ffprobe', default='ffprobe')
    args = parser.parse_args()
    try:
        result = judge(args.recording, args.trace, args.source_id, args.start_ticks, args.end_ticks,
                       args.minimum_seconds, args.ffmpeg, args.ffprobe)
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if result['isoPixelArrivalFidelityPassed'] else 1)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({'isoPixelArrivalFidelityPassed': False, 'error': str(error)}))
        raise SystemExit(2)
