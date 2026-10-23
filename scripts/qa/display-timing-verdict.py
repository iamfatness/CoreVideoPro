"""Judge a declared PresentMon interval against immutable named native leases.

Requires actual display-change timestamps, not accepted Present calls. Source
pixels, native delivery and receivers remain separate qualification boundaries.
"""
import csv
import math


def chain(value):
    return int(str(value), 16)


def judge(path, manifest, observations):
    start, end = int(manifest['startQpc']), int(manifest['endQpc'])
    frequency = int(manifest['qpcFrequency'])
    minimum = float(manifest['minimumSeconds'])
    pid = int(manifest['processId'])
    surfaces = manifest['surfaces']
    if frequency <= 0 or end <= start or minimum < 1 or pid <= 0 or not surfaces:
        raise ValueError('invalid or missing declared interval/process/surface inventory')
    if (end - start) / frequency < minimum:
        raise ValueError('declared interval shorter than required qualification')
    if manifest['refreshNumerator'] != 60 * manifest['refreshDenominator'] or manifest['refreshDenominator'] <= 0:
        raise ValueError('declared display mode is not exact 60 Hz')
    addresses = {name: chain(row['swapChainAddress']) for name, row in surfaces.items()}
    if 0 in addresses.values() or len(set(addresses.values())) != len(addresses):
        raise ValueError('missing or reused named swap-chain address')
    errors, results = [], {}
    for name, address in addresses.items():
        rows = sorted((row for row in observations if row.get('label') == name), key=lambda row: int(row['observedQpc']))
        before = [row for row in rows if int(row['observedQpc']) <= start]
        after = [row for row in rows if int(row['observedQpc']) >= end]
        if not before or not after:
            errors.append(f'{name}: native lease observations do not cover interval')
        else:
            selected = [row for row in rows if int(before[-1]['observedQpc']) <= int(row['observedQpc']) <= int(after[0]['observedQpc'])]
            if any(row.get('backend') != 'native-independent' or chain(row['swapChainAddress']) != address or
                   row['chainRevision'] != surfaces[name]['chainRevision'] for row in selected):
                errors.append(f'{name}: native lease changed or backend unverified')
        results[name] = {'presents': 0, 'notDisplayed': 0, 'displayQpc': []}
    by_address = {address: name for name, address in addresses.items()}
    with open(path, encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            if int(row['ProcessID']) != pid:
                continue
            name = by_address.get(chain(row['SwapChainAddress']))
            if name is None:
                continue
            timestamp = int(row['TimeInQPC'])
            result = results[name]
            if start <= timestamp <= end:
                result['presents'] += 1
                if row['MsUntilDisplayed'] == 'NA':
                    result['notDisplayed'] += 1
            if row['MsUntilDisplayed'] != 'NA':
                delay = float(row['MsUntilDisplayed'])
                if not math.isfinite(delay) or delay < 0:
                    raise ValueError('invalid physical display delay')
                displayed = timestamp + round(delay * frequency / 1000)
                if start <= displayed <= end:
                    result['displayQpc'].append(displayed)
    expected = round((end - start) * 60 / frequency)
    for name, result in results.items():
        times = sorted(result.pop('displayQpc'))
        intervals = [(b - a) * 1000 / frequency for a, b in zip(times, times[1:])]
        edges = [(times[0] - start) * 1000 / frequency, (end - times[-1]) * 1000 / frequency] if times else []
        result.update(displayed=len(times), expected=expected, maximumIntervalMs=max(intervals, default=None))
        if result['notDisplayed']:
            errors.append(f'{name}: submitted video frames were not displayed')
        if not times or abs(len(times) - expected) > 1:
            errors.append(f'{name}: displayed frame density differs from 60/1')
        if any(value <= 0 or value > 25 for value in intervals) or any(value > 25 for value in edges):
            errors.append(f'{name}: duplicate display timestamp or missed display interval')
    return {'result': 'FAIL' if errors else 'PASS', 'processId': pid, 'surfaces': results,
            'measuredSeconds': (end - start) / frequency, 'errors': sorted(set(errors)),
            'scope': 'Named native video leases and PresentMon physical display timing only; source pixels, receiver and full workload are separate.'}
