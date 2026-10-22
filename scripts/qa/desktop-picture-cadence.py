"""Audit identities in Desktop Duplication images; never infer physical scanout.

Complete desktop-update coverage can establish a missing source picture before
scanout. Repeated identities alone do not establish repeated physical refreshes.
"""
import argparse
import csv
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('pictures', Path(__file__).with_name('source-picture-verdict.py'))
pictures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pictures)


def audit(rows, groups, start, end, point_count):
    if end <= start or point_count <= 0:
        raise ValueError('Invalid interval or point count')
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
    surfaces = {}
    for group in groups:
        # Body samples may have the same name; they are not identity markers.
        if group['category'] != 'markers' or group['name'] not in ('previewCamera', 'programCamera', 'inputCamera'):
            continue
        name, offset, length = group['name'], group['start'], group['length']
        if name in surfaces or length != 72 or offset < 0 or offset + length > point_count:
            raise ValueError('Duplicate or invalid camera marker group')
        values = [pictures.decode_bands(value[offset:offset + length]) for value in raw]
        transitions = [(x, y) for x, y in zip(values, values[1:]) if x is not None and y is not None]
        surfaces[name] = dict(samples=len(values), invalid=sum(value is None for value in values),
                              missingSourceIdentities=sum(max(0, y - x - 1) for x, y in transitions),
                              reordered=sum(y < x for x, y in transitions),
                              sameImageUpdates=sum(y == x for x, y in transitions))
    errors = list(coverage)
    for name in ('previewCamera', 'programCamera', 'inputCamera'):
        surface = surfaces.get(name)
        if surface is None or any(surface[key] for key in ('invalid', 'missingSourceIdentities', 'reordered')):
            errors.append('Missing, invalid, omitted or reordered source pictures: ' + name)
    return dict(schema='desktop-picture-cadence-v1', observerCoverageComplete=not coverage,
                sourceIdentityContinuityVerified=not errors, surfaces=surfaces, errors=errors,
                physicalDisplayCadenceVerified=False, fullWorkloadPassed=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trial', type=Path)
    parser.add_argument('output', type=Path)
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
                   int(measurement['start']['qpc']), int(measurement['end']['qpc']), manifest['points'])
    report.update(startTicks=measurement['start']['qpc'], endTicks=measurement['end']['qpc'], qpcFrequency=frequency)
    args.output.write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    raise SystemExit(not report['sourceIdentityContinuityVerified'])
