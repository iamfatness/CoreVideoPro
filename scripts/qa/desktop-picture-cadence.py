"""Audit identities in Desktop Duplication images; never infer physical scanout.

Complete desktop-update coverage can establish a missing source picture before
scanout. Repeated identities alone do not establish repeated physical refreshes.

This is the Studio UX metric, not the release gate (CLAUDE.md, owner ruling
2026-10-22). The on-screen cells are windowed surfaces composed by the Desktop
Window Manager; on a mixed-refresh multi-monitor desk Windows skips a frame at
the beat between monitor clocks for every application, so the cells are judged
against a tolerance. Outputs (recordings, OS camera, stream) are judged with
zero tolerance by source-picture-verdict.py and iso-pixel-verdict.py.
"""
import argparse
import csv
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('pictures', Path(__file__).with_name('source-picture-verdict.py'))
pictures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pictures)

SURFACES = ('previewCamera', 'programCamera', 'inputCamera')
DEFAULT_MAX_HOLD_MS = 100.0
DEFAULT_MAX_OMISSION_RATIO = 0.01


def longest_hold_ms(times, values, frequency):
    """Longest time the composed desktop showed one source identity.

    A hold spans from the first sample of an identity to the first sample of a
    different valid identity. Invalid samples neither start nor end a hold.
    """
    longest = 0.0
    hold_start = None
    held = None
    for time, value in zip(times, values):
        if value is None:
            continue
        if held is None or value != held:
            if hold_start is not None:
                longest = max(longest, (time - hold_start) * 1000.0 / frequency)
            hold_start, held = time, value
    return longest


def audit(rows, groups, start, end, point_count, qpc_frequency=10_000_000,
          max_hold_ms=DEFAULT_MAX_HOLD_MS, max_omission_ratio=DEFAULT_MAX_OMISSION_RATIO):
    if end <= start or point_count <= 0 or qpc_frequency <= 0:
        raise ValueError('Invalid interval, point count or clock frequency')
    times = [int(row['lastPresentQpc']) for row in rows]
    coverage = []
    if not times or times != sorted(set(times)):
        coverage.append('Desktop update timestamps absent, repeated or reordered')
    if not times or times[0] > start or times[-1] < end:
        coverage.append('Desktop observer does not bracket the declared interval')
    selected = [row for row in rows if start <= int(row['lastPresentQpc']) <= end]
    if not selected:
        coverage.append('No observed desktop pictures in interval')
    if any(int(row['accumulatedFrames']) != 1 for row in selected):
        coverage.append('Desktop updates accumulated or coverage count is invalid')
    if any(int(row['protectedContent']) != 0 for row in selected):
        coverage.append('Protected desktop pixels were masked')
    raw = [bytes.fromhex(row['pixels']) for row in selected]
    if any(len(value) != point_count for value in raw):
        raise ValueError('Desktop sample point count changed')
    selected_times = [int(row['lastPresentQpc']) for row in selected]
    surfaces = {}
    for group in groups:
        # Body samples may have the same name; they are not identity markers.
        if group['category'] != 'markers' or group['name'] not in SURFACES:
            continue
        name, offset, length = group['name'], group['start'], group['length']
        if name in surfaces or length != 72 or offset < 0 or offset + length > point_count:
            raise ValueError('Duplicate or invalid camera marker group')
        values = [pictures.decode_bands(value[offset:offset + length]) for value in raw]
        transitions = [(x, y) for x, y in zip(values, values[1:]) if x is not None and y is not None]
        valid = [value for value in values if value is not None]
        missing = sum(max(0, y - x - 1) for x, y in transitions)
        expected = (valid[-1] - valid[0]) if len(valid) > 1 else 0
        ratio = (missing / expected) if expected > 0 else 0.0
        hold = longest_hold_ms(selected_times, values, qpc_frequency)
        surfaces[name] = dict(samples=len(values), invalid=sum(value is None for value in values),
                              missingSourceIdentities=missing,
                              reordered=sum(y < x for x, y in transitions),
                              sameImageUpdates=sum(y == x for x, y in transitions),
                              omissionRatio=ratio, longestHoldMs=hold,
                              withinTolerance=ratio <= max_omission_ratio and hold <= max_hold_ms)
    errors = list(coverage)
    observations = []
    for name in SURFACES:
        surface = surfaces.get(name)
        if surface is None:
            errors.append('Missing source-picture marker group: ' + name)
            continue
        if surface['invalid'] or surface['reordered']:
            errors.append('Invalid or reordered source pictures: ' + name)
        if not surface['withinTolerance']:
            errors.append('Studio picture tolerance exceeded (hold %.1f ms, omission %.2f%%): %s'
                          % (surface['longestHoldMs'], surface['omissionRatio'] * 100.0, name))
        if surface['missingSourceIdentities']:
            observations.append('%d omitted source identities on %s (composition, not a release failure)'
                                % (surface['missingSourceIdentities'], name))
    continuity = not coverage and all(
        s is not None and not any(s[key] for key in ('invalid', 'missingSourceIdentities', 'reordered'))
        for s in (surfaces.get(name) for name in SURFACES))
    return dict(schema='desktop-picture-cadence-v2', observerCoverageComplete=not coverage,
                sourceIdentityContinuityVerified=continuity,
                studioPicturesWithinTolerance=not errors,
                tolerance=dict(maxHoldMs=max_hold_ms, maxOmissionRatio=max_omission_ratio),
                surfaces=surfaces, errors=errors, observations=observations,
                releaseGate=False,
                physicalDisplayCadenceVerified=False, fullWorkloadPassed=False,
                scope='Studio UX metric over composed desktop pixels. Not a release gate: outputs are '
                      'judged with zero tolerance by source-picture-verdict.py and iso-pixel-verdict.py.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trial', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--max-hold-ms', type=float, default=DEFAULT_MAX_HOLD_MS)
    parser.add_argument('--max-omission-ratio', type=float, default=DEFAULT_MAX_OMISSION_RATIO)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Preserve previous evidence')
    read = lambda name: json.loads((args.trial / name).read_text(encoding='utf-8-sig'))
    measurement, manifest = read('measurement.json'), read('desktop-updates.manifest.json')
    result = read('desktop-updates.result.json')
    lines = (args.trial / 'desktop-updates.csv').read_text().splitlines()
    if result['exitCode'] or not lines[-1].startswith('#complete,'):
        raise ValueError('Desktop observer failed or did not finalize')
    frequency = int(lines[0].split('qpcFrequency=')[1].split(',')[0])
    if frequency != int(read('observer-baseline.json')['qpcFrequency']):
        raise ValueError('Observer and workload clocks differ')
    report = audit(list(csv.DictReader(lines[1:-1])), manifest['groups'],
                   int(measurement['start']['qpc']), int(measurement['end']['qpc']), manifest['points'],
                   qpc_frequency=frequency, max_hold_ms=args.max_hold_ms,
                   max_omission_ratio=args.max_omission_ratio)
    report.update(startTicks=measurement['start']['qpc'], endTicks=measurement['end']['qpc'], qpcFrequency=frequency)
    args.output.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
