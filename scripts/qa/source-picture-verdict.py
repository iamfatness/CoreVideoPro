"""Judge independently sampled source pixels, separately from Program counters.

The manifest fixes sampling coordinates and intervals before the measured run.
Camera bits were painted into captured pixels before preparation. Screen bits
come from an independent desktop stimulus. Ordinary picture samples detect
blank/slate output and provide a second view of actual content progression.
"""
import bisect
import importlib.util
import json
import pathlib
import struct


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
    invalid = mismatched = stale = 0
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
    previous, changes, longest_hold, hold_start = None, 0, 0, None
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
    if not changes or longest_hold > manifest['maximumCameraBodyHoldMs']:
        errors.append('Ordinary camera picture progression did not meet declared observation bound')
    return {'schema': 'source-picture-verdict-v1', 'sourcePicturesVerified': not errors,
            'samples': len(measured), 'invalid': invalid, 'mismatched': mismatched, 'stale': stale,
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
