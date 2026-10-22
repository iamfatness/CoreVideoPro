"""Judge independently sampled source pixels, separately from Program counters.

The manifest fixes sampling coordinates and intervals before the measured run.
Camera bits were painted into captured pixels before preparation. Screen bits
come from an independent desktop stimulus. Ordinary picture samples detect
blank/slate output and provide a second view of actual content progression.
"""
import bisect
import json
import pathlib
import struct
import subprocess
import tempfile


def decode_bands(values):
    if len(values) != 72:
        return None
    identity = 0
    for cell, (top, bottom) in enumerate(zip(values[:36], values[36:])):
        if not ((top < 64 and bottom > 192) or (top > 192 and bottom < 64)):
            return None
        bit = top > 192
        if cell in (0, 1, 34, 35):
            if bit != (cell in (0, 35)): return None
        elif bit:
            identity |= 1 << (cell - 2)
    return identity


def judge_samples(rows, manifest, camera_draws, stimulus):
    errors, camera_ages, screen_ages = [], [], []
    invalid = mismatched = stale = repeated = missing = reordered = 0
    start, end = int(manifest['startQpc']), int(manifest['endQpc'])
    frequency = int(manifest['qpcFrequency'])
    if frequency <= 0 or end <= start: raise ValueError('Invalid measurement clock')
    minimum = manifest['minimumSeconds']
    if (end - start) / frequency < minimum: raise ValueError('Insufficient declared duration')
    points = manifest['points']
    if not 144 <= len(points) <= 256: raise ValueError('Missing or excessive picture points')
    if len(manifest['cameraMarker']) != 72 or len(manifest['screenMarker']) != 72:
        raise ValueError('Both source markers required')
    if len(manifest['cameraBody']) < 16: raise ValueError('Ordinary camera picture samples required')
    times = [r['afterNs'] for r in stimulus]
    by_identity = {r['identity']: r for r in stimulus}
    if times != sorted(set(times)) or len(by_identity) != len(times) or not times:
        raise ValueError('Invalid independent stimulus sequence')
    measured = [r for r in rows if 'sample' in r and start <= r['arrivalHostUs'] * frequency // 1_000_000 <= end]
    if len(measured) < minimum * 60: errors.append('Insufficient received picture coverage')
    previous, previous_camera, changes, longest_hold, hold_start = None, None, 0, 0, None
    for row in measured:
        raw = bytes.fromhex(row.get('pictureLuma', ''))
        if len(raw) != len(points):
            invalid += 1; continue
        camera = decode_bands([raw[i] for i in manifest['cameraMarker']])
        screen = decode_bands([raw[i] for i in manifest['screenMarker']])
        body = bytes(raw[i] for i in manifest['cameraBody'])
        arrival_ns = row['arrivalHostUs'] * 1000
        drawn = camera_draws.get(row.get('identity'))
        if camera is None or screen is None or drawn is None:
            invalid += 1; continue
        if camera != drawn['sourceFrameId'] or drawn['reason'] != 1:
            mismatched += 1
        if previous_camera is not None:
            delta = camera - previous_camera
            repeated += delta == 0
            missing += max(0, delta - 1)
            reordered += delta < 0
        previous_camera = camera
        # Observation-to-consumer age, not sensor exposure latency.
        camera_age = (arrival_ns - drawn['observation100ns'] * 100) / 1e6
        camera_ages.append(camera_age)
        selected_stimulus = by_identity.get(screen)
        if selected_stimulus is None or selected_stimulus['beforeNs'] > arrival_ns:
            invalid += 1; continue
        screen_age = (arrival_ns - selected_stimulus['beforeNs']) / 1e6
        screen_ages.append(screen_age)
        latest = bisect.bisect_right(times, arrival_ns) - 1
        if latest < 0 or stimulus[latest]['identity'] - screen > manifest['maximumScreenPictureLag']:
            stale += 1
        if camera_age < 0 or camera_age > manifest['maximumCameraObservationAgeMs'] or screen_age > manifest['maximumScreenAgeMs']:
            stale += 1
        if max(body) - min(body) < 12: invalid += 1
        if previous is None or body != previous:
            changes += previous is not None
            hold_start = arrival_ns
        longest_hold = max(longest_hold, (arrival_ns - hold_start) / 1e6)
        previous = body
    if invalid: errors.append(f'{invalid} missing, torn, blank or unattributable source pictures')
    if mismatched: errors.append(f'{mismatched} camera pictures differ from the exact drawn source identity')
    if stale: errors.append(f'{stale} stale source pictures')
    source_rate = manifest['cameraRateNumerator'] / manifest['cameraRateDenominator']
    if not 0 < source_rate <= 60: raise ValueError('Declare the actual negotiated camera rate, up to 60 Hz')
    # Native lower-rate sources legitimately repeat at 60 Hz. Captured identity
    # gaps are still loss; sampling jitter must not be dismissed as conversion.
    import math
    permitted_repeats = math.ceil((len(measured) - 1) * (1 - source_rate / 60)) + (2 if source_rate < 60 else 0)
    if missing or reordered or repeated > permitted_repeats:
        errors.append(f'Camera picture cadence failed: repeated={repeated}, permitted={permitted_repeats}, missing={missing}, reordered={reordered}')
    if not changes or longest_hold > manifest['maximumCameraBodyHoldMs']:
        errors.append('Ordinary camera picture progression did not meet declared observation bound')
    return {'schema': 'source-picture-verdict-v1', 'sourcePicturesVerified': not errors,
            'startTicks': str(start), 'endTicks': str(end), 'qpcFrequency': frequency,
            'durationSeconds': (end - start) / frequency,
            'cameraSourceId': manifest['cameraSourceId'], 'screenSourceId': manifest['screenSourceId'],
            'cameraRateNumerator': manifest['cameraRateNumerator'], 'cameraRateDenominator': manifest['cameraRateDenominator'],
            'samples': len(measured), 'invalid': invalid, 'mismatched': mismatched, 'stale': stale,
            'cameraRepeated': repeated, 'cameraMissing': missing, 'cameraReordered': reordered,
            'cameraPermittedRateConversionRepeats': permitted_repeats,
            'cameraBodyChanges': changes, 'longestCameraBodyHoldMs': longest_hold,
            'maximumCameraObservationAgeMs': max(camera_ages, default=None),
            'maximumScreenPictureAgeMs': max(screen_ages, default=None), 'errors': errors,
            'limitation': 'Actual received source pixels within declared bounds; camera observation time is not sensor exposure time. Physical display and native acquisition cadence require separate evidence.'}


def camera_draws(path, source_id):
    header, event = struct.Struct('<8sIIQQQQQQqq'), struct.Struct('<QQQqqqqQIIqq')
    result = {}
    with pathlib.Path(path).open('rb') as stream:
        h = header.unpack(stream.read(header.size))
        if h[0:3] != (b'CVTRACE2', 2, event.size) or h[6] or h[7] or h[8] != 1:
            raise ValueError('Finalized lossless V2 trace required')
        if pathlib.Path(path).stat().st_size != header.size + h[5] * event.size:
            raise ValueError('Trace size mismatch')
        tag = 14695981039346656037 ^ h[4]
        for byte in source_id.encode(): tag = ((tag ^ byte) * 1099511628211) & 0xffffffffffffffff
        for _ in range(h[5]):
            r = event.unpack(stream.read(event.size))
            if r[8] == 2 and r[1] == tag:
                if r[3] in result: raise ValueError('Ambiguous source draws for Program identity')
                result[r[3]] = {'sourceFrameId': r[4], 'observation100ns': r[6], 'reason': r[9]}
    return result


def recording_samples(path, ffmpeg, points, receiver_rows):
    """Decode every encoded frame; read only declared luma rows from FFmpeg."""
    width = 1920
    ys = sorted({16, 1064, *(p[1] for p in points)})
    count = len(ys)
    labels = ''.join(f'[r{i}]' for i in range(count))
    crops = ';'.join(f'[r{i}]crop=1920:1:0:{y}[c{i}]' for i, y in enumerate(ys))
    filtered = f'format=gray,split={count}{labels};{crops};' + ''.join(f'[c{i}]' for i in range(count)) + f'vstack=inputs={count}'
    offsets = {y: i * width for i, y in enumerate(ys)}
    by_program = {r['identity']: r for r in receiver_rows if isinstance(r.get('identity'), int)}
    result = []
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([ffmpeg, '-nostdin', '-v', 'error', '-i', str(path), '-filter_complex', filtered,
                                    '-an', '-fps_mode', 'passthrough', '-enc_time_base', '1/10000000',
                                    '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], stdout=subprocess.PIPE, stderr=errors)
        try:
            for frame in range(2_000_000):
                raw = process.stdout.read(width * count)
                if not raw: break
                if len(raw) != width * count: raise ValueError('Partial decoded recording picture')
                marker = [raw[offsets[y] + cell * 52 + 26] for y in (16, 1064) for cell in range(36)]
                identity = decode_bands(marker)
                reference = by_program.get(identity)
                if reference is not None:
                    result.append({'sample': frame + 1, 'identity': identity,
                                   'arrivalHostUs': reference['arrivalHostUs'],
                                   'pictureLuma': bytes(raw[offsets[y] + x] for x, y in points).hex()})
            else: raise ValueError('Recording exceeds bounded decoder capacity')
            code = process.wait(timeout=30)
            errors.seek(0)
            detail = errors.read(4096).decode(errors='replace')
            if code or detail: raise ValueError(f'Recording decoder failed: {code} {detail}')
        finally:
            if process.poll() is None: process.kill(); process.wait()
    return result


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest'); parser.add_argument('receiver'); parser.add_argument('trace')
    parser.add_argument('stimulus'); parser.add_argument('output')
    parser.add_argument('--recording'); parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    output = pathlib.Path(args.output)
    if output.exists(): raise ValueError('Preserve existing verdict')
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding='utf-8-sig'))
    rows = [json.loads(line) for line in pathlib.Path(args.receiver).read_text(encoding='utf-8-sig').splitlines()]
    if rows[0].get('picturePoints') != manifest['points'] or rows[0].get('receiverMode') != 'os-camera' or not rows[-1].get('complete'):
        raise ValueError('Complete registered camera capture with exact declared coordinates required')
    stimulus = [json.loads(line) for line in pathlib.Path(args.stimulus).read_text(encoding='utf-8-sig').splitlines()]
    if not stimulus[-1].get('complete'): raise ValueError('Independent stimulus did not finalize')
    selected = [r for r in stimulus if 'identity' in r]
    draws = camera_draws(args.trace, manifest['cameraSourceId'])
    report = {'osCamera': judge_samples(rows, manifest, draws, selected), 'fullWorkloadPassed': False,
              'trace': args.trace, 'receiver': args.receiver, 'recordingPath': args.recording}
    if args.recording:
        decoded = recording_samples(args.recording, args.ffmpeg, manifest['points'], rows)
        report['recording'] = judge_samples(decoded, manifest, draws, selected)
    output.write_text(json.dumps(report, indent=2), encoding='utf8')
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['osCamera']['sourcePicturesVerified'] and
                     (not args.recording or report['recording']['sourcePicturesVerified']) else 1)
