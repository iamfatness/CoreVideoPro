"""Offline per-output, per-frame verdict for scripts/qa/outputs-drill.ps1 (#443).

Run after the live collection has stopped every output and the app has closed,
so decoding never competes with the workload. Adapted from the 2026-10-22
qualification harness (artifacts/qualification-1008-25/Finalize-DeclaredInterval.py),
which passed a 30-minute soak. The judging is NOT redesigned here: every output
is judged by the existing modules in scripts/qa/:

  Program recording, stream  -> recording-pixel-verdict.py  (Program QA counter)
  each ISO                   -> iso-pixel-verdict.py        (trace stage 11 arrivals)
  registered OS camera       -> camera-pixel-receiver.mjs   (independent receiver)
  Program source admissions  -> program-source-admission-capture.mjs (gates Program)
  Program GPU deadlines      -> delivery-trace.py           (supplementary only)

The live collector's throughout-assertions (hardware encoders, remotes at
1080p, capture progress, Program admissions, stream health) arrive in
measurement.json; any false one fails the drill, as in Collect-Identity.ps1.

This file only orchestrates them and aggregates their results into
verdict.json and summary.txt. An output whose judge could not run is NOT JUDGED
with the reason; it is never PASS. Overall PASS requires every enabled output to
PASS (zero missing, repeated, reordered or invalid identities) inside an
interval bounded by the OS camera; without that bound Program and stream are
UNBOUNDED, never PASS.
"""
import argparse
import importlib.util
import json
import pathlib
import subprocess
import sys
import traceback

HERE = pathlib.Path(__file__).resolve().parent
PASS, FAIL, NOT_JUDGED = 'PASS', 'FAIL', 'NOT JUDGED'
# Clean identities over the whole file, but no declared interval bounded them
# (no OS camera). The reference refused to judge without bounds; this status
# reports the counts and can never make the overall verdict PASS.
UNBOUNDED = 'UNBOUNDED'
THROUGHOUT_FLAGS = ('hardwareEncodersThroughout', 'remotes1080Throughout', 'captureProgressThroughout',
                    'programSourcesAdmittedThroughout', 'streamHealthyThroughout')


def load_module(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


# ---------------------------------------------------------------- pure rows

def not_judged(output, kind, reason, source_id=None):
    return {'output': output, 'kind': kind, 'sourceId': source_id, 'status': NOT_JUDGED,
            'expected': None, 'decoded': None, 'missing': None, 'repeated': None,
            'reordered': None, 'invalid': None, 'reason': reason, 'proof': None}


def _judge_error(verdict):
    """A judge that raised is recorded as {'error': ...}; it proved nothing."""
    return isinstance(verdict, dict) and 'error' in verdict and len(verdict) <= 2


def recording_row(output, kind, verdict, proof, bounded):
    """Program recording or stream, judged by recording-pixel-verdict.py."""
    if verdict is None:
        return not_judged(output, kind, 'no judge output')
    if _judge_error(verdict):
        return not_judged(output, kind, 'judge could not run: ' + str(verdict['error']))
    first, last = verdict.get('requestedFirstIdentity'), verdict.get('requestedLastIdentity')
    if first is None or last is None:
        first, last = verdict.get('firstIdentity'), verdict.get('lastIdentity')
    expected = (last - first + 1) if isinstance(first, int) and isinstance(last, int) else None
    passed = verdict.get('recordedPixelContinuityPassed') is True and verdict.get('missing') == 0
    reason = None
    if not passed:
        problems = [f'{key}={verdict.get(key)}' for key in ('missing', 'repeated', 'reordered', 'invalid', 'rangeViolations')
                    if verdict.get(key)]
        if verdict.get('identityRangeComplete') is False:
            problems.append('identity range incomplete')
        if verdict.get('decoderComplete') is False or verdict.get('decoderErrors'):
            problems.append('decoder did not complete cleanly')
        if not problems:
            problems.append(f"samples={verdict.get('samples')} below the {verdict.get('minimumSeconds')} s minimum")
        reason = '; '.join(problems)
    elif not bounded:
        reason = 'whole-file identity range only: no OS camera interval bounded it, so it cannot PASS'
    status = (PASS if bounded else UNBOUNDED) if passed else FAIL
    return {'output': output, 'kind': kind, 'sourceId': None, 'status': status,
            'expected': expected, 'decoded': verdict.get('samples'), 'missing': verdict.get('missing'),
            'repeated': verdict.get('repeated'), 'reordered': verdict.get('reordered'),
            'invalid': verdict.get('invalid'), 'reason': reason, 'proof': proof}


def iso_row(source_id, verdict, proof):
    output = 'ISO ' + source_id
    if verdict is None:
        return not_judged(output, 'iso', 'no judge output', source_id)
    if _judge_error(verdict) or 'expectedSamples' not in verdict:
        return not_judged(output, 'iso', 'judge could not run: ' + str(verdict.get('error')), source_id)
    passed = verdict.get('isoPixelArrivalFidelityPassed') is True and verdict.get('missing', 1) == 0
    held = verdict.get('heldExcused') or 0
    reason = None
    if not passed:
        bits = [f"{key}={verdict.get(key)}" for key in ('missing', 'repeated', 'invalid', 'rangeViolations') if verdict.get(key)]
        if verdict.get('holdsMatched') is False:
            bits.append(f"held frames: writer reported {verdict.get('heldFrames')}, file shows {verdict.get('heldExcused')}")
        if verdict.get('firstMismatchIndex') is not None:
            bits.append(f"first mismatch at index {verdict['firstMismatchIndex']}")
        if verdict.get('decoderComplete') is False or verdict.get('decoderErrors'):
            bits.append('decoder did not complete cleanly')
        reason = '; '.join(bits) or 'decoded identities differ from offered arrivals'
    elif held:
        # #1030: a PASS, but never a silent one: the source's video stalled and
        # the writer held its last picture. The stall length is in the ISO
        # judge's coverage.maximumSourceArrivalGapMs.
        gap = (verdict.get('coverage') or {}).get('maximumSourceArrivalGapMs')
        reason = f"held={held}, source stalled" + (f" (longest arrival gap {gap:.0f} ms)" if gap else '')
    return {'output': output, 'kind': 'iso', 'sourceId': source_id, 'status': PASS if passed else FAIL,
            'held': held,
            'expected': verdict.get('expectedSamples'), 'decoded': verdict.get('recordedSamples'),
            'missing': verdict.get('missing'), 'repeated': verdict.get('repeated'), 'reordered': None,
            'invalid': verdict.get('invalid'), 'reason': reason, 'proof': proof}


def camera_row(verdict, proof, covers_interval, serving_proven):
    output = 'OS virtual camera'
    if verdict is None:
        return not_judged(output, 'os-camera', 'no judge output')
    if _judge_error(verdict):
        return not_judged(output, 'os-camera', 'judge could not run: ' + str(verdict['error']))
    if not serving_proven:
        return not_judged(output, 'os-camera', 'registered camera was not proven to serve the installed DLL '
                          '(camera-serving-identity.json missing)')
    first, last = verdict.get('firstIdentity'), verdict.get('lastIdentity')
    expected = (last - first + 1) if isinstance(first, int) and isinstance(last, int) else None
    passed = verdict.get('osCameraContinuityVerified') is True and verdict.get('missing') == 0 and covers_interval
    reason = None
    if not passed:
        reason = '; '.join(verdict.get('errors') or []) or ('receiver did not cover the measured interval'
                                                           if not covers_interval else 'continuity not verified')
    return {'output': output, 'kind': 'os-camera', 'sourceId': None, 'status': PASS if passed else FAIL,
            'expected': expected, 'decoded': verdict.get('samples'), 'missing': verdict.get('missing'),
            'repeated': verdict.get('repeated'), 'reordered': verdict.get('reordered'),
            'invalid': verdict.get('invalid'), 'reason': reason, 'proof': proof}


def gate_program_admissions(row, admissions):
    """Finalize-DeclaredInterval.py: Program pixels count only when the Program
    source images were admitted throughout (program-source-admission-capture.mjs)."""
    if row['status'] not in (PASS, UNBOUNDED):
        return row
    if admissions is None or _judge_error(admissions) or 'sourceAdmissionsVerified' not in admissions:
        error = (admissions or {}).get('error', 'no admission evidence')
        return {**row, 'status': NOT_JUDGED, 'reason': 'Program source admission capture could not run: ' + str(error)}
    if admissions.get('sourceAdmissionsVerified') is not True:
        return {**row, 'status': FAIL, 'reason': (f"Program source admissions not verified throughout: samples={admissions.get('samples')} "
                                                   f"failed={admissions.get('failedSamples')} missingEvidence={admissions.get('missingEvidenceSamples')}")}
    return row


def throughout_failures(measurement):
    """Collect-Identity.ps1 throughout-assertions recorded in measurement.json."""
    return [flag for flag in THROUGHOUT_FLAGS if flag in measurement and measurement[flag] is not True]


def aggregate(rows, context):
    """Overall verdict. PASS only when every enabled output PASSes."""
    failure = context.get('collectionFailure')
    broken = context.get('throughoutFailures') or []
    if failure or broken:
        overall = FAIL
    elif not rows:
        overall = NOT_JUDGED
    elif all(row['status'] == PASS for row in rows):
        overall = PASS
    elif any(row['status'] == FAIL for row in rows):
        overall = FAIL
    else:
        overall = NOT_JUDGED
    notes = list(context.get('notes', []))
    if broken:
        notes.insert(0, 'Workload assertions failed during the interval: ' + ', '.join(broken))
    if failure:
        notes.insert(0, 'Live collection aborted: ' + failure)
    return {'schema': 'outputs-drill-verdict-v2', 'overall': overall,
            'rule': 'PASS only if every enabled output PASSes with zero missing, repeated, reordered or invalid identities '
                    'inside an OS-camera-bounded interval, and every workload assertion held throughout; '
                    'NOT JUDGED and UNBOUNDED never count as PASS.',
            'throughoutFailures': broken,
            'seconds': context.get('seconds'), 'workload': context.get('workload'),
            'outputs': rows, 'skipped': context.get('skipped', []),
            'supplementary': context.get('supplementary', {}), 'notes': notes,
            'scope': 'Output pixel identity only. On-screen Studio cells, audio and physical display are not judged here.'}


def summary_text(verdict):
    def number(value):
        return '-' if value is None else str(value)
    lines = [f"Outputs drill: {verdict['overall']}  ({verdict.get('seconds')} s, {verdict.get('workload')})", '']
    width = max([len(row['output']) for row in verdict['outputs']] + [16])
    for row in verdict['outputs']:
        line = (f"{row['output']:<{width}}  {row['status']:<10}  expected={number(row['expected'])} "
                f"decoded={number(row['decoded'])} missing={number(row['missing'])} repeated={number(row['repeated'])}")
        if row.get('reason'):
            line += f"  ({row['reason']})"
        lines.append(line)
    for skipped in verdict.get('skipped', []):
        lines.append(f"{skipped:<{width}}  SKIPPED")
    deadlines = verdict.get('supplementary', {}).get('programGpuDeadlines')
    if deadlines:
        lines.append(f"{'Program GPU deadlines':<{width}}  {deadlines}  (supplementary; not part of the overall verdict)")
    if verdict.get('notes'):
        lines.append('')
        lines.extend('Note: ' + note for note in verdict['notes'])
    return '\n'.join(lines) + '\n'


def bounded_camera_identities(rows, start_us, end_us):
    """Receiver samples whose host arrival lies inside the declared interval."""
    bounded = [row for row in rows if 'sample' in row and start_us <= row.get('arrivalHostUs', -1) <= end_us]
    return (bounded[0]['identity'], bounded[-1]['identity'], len(bounded)) if bounded else None


# ---------------------------------------------------------------- live judging

def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def run(evidence):
    drill = read_json(evidence / 'drill.json')
    judges = evidence / 'judges'
    judges.mkdir(exist_ok=True)
    seconds = int(drill['seconds'])
    tools = drill['tools']
    enabled = drill['outputs']
    notes = list(drill.get('notes', []))
    skipped = [name for name, on in (('Stream', enabled.get('stream')), ('OS virtual camera', enabled.get('camera'))) if not on]
    context = {'seconds': seconds, 'workload': drill.get('workload'), 'skipped': skipped, 'notes': notes, 'supplementary': {}}

    def save(name, value):
        (judges / name).write_text(json.dumps(value, indent=2), encoding='utf-8')
        return 'judges/' + name

    def attempt(name, call):
        print(f'Judging {name}', flush=True)
        try:
            value = call()
        except Exception as error:  # a judge that raised proved nothing
            value = {'error': f'{type(error).__name__}: {error}'}
        return value, save(name, value)

    planned = [('Program recording', 'program')] + [('ISO ' + s, 'iso') for s in enabled.get('iso', [])]
    if enabled.get('camera'):
        planned.append(('OS virtual camera', 'os-camera'))
    if enabled.get('stream'):
        planned.append(('Stream', 'stream'))

    failure_file = evidence / 'collection-failure.json'
    if failure_file.exists():
        context['collectionFailure'] = read_json(failure_file).get('error', 'unknown')
        rows = [not_judged(name, kind, 'live collection aborted before the interval completed') for name, kind in planned]
        return aggregate(rows, context)
    for required in ('measurement.json', 'finalized.json'):
        if not (evidence / required).exists():
            context['collectionFailure'] = f'{required} was not written'
            return aggregate([not_judged(name, kind, f'{required} missing') for name, kind in planned], context)

    measurement = read_json(evidence / 'measurement.json')
    final = read_json(evidence / 'finalized.json')
    start, end = int(measurement['start']['qpc']), int(measurement['end']['qpc'])
    frequency = int(measurement['qpcFrequency'])
    context['throughoutFailures'] = throughout_failures(measurement)
    missing_flags = [flag for flag in THROUGHOUT_FLAGS[:4] if flag not in measurement]
    if missing_flags:
        context['throughoutFailures'] += [flag + ' (not recorded)' for flag in missing_flags]
    trace = evidence / 'arrival-trace.bin'
    ffmpeg, ffprobe = tools['ffmpeg'], tools['ffprobe']
    finalized_ok = bool(final.get('recording', {}).get('lifecycle', {}).get('finalized'))
    trace_ok = bool(final.get('deliveryEvidence', {}).get('captureComplete')) and trace.exists()
    record_module = load_module('record_pixels', 'recording-pixel-verdict.py')
    iso_module = load_module('iso_pixels', 'iso-pixel-verdict.py')
    rows = []

    # OS camera first: its bounded identities bound the Program recording and stream.
    bounds = None
    camera = None
    if enabled.get('camera'):
        receiver = evidence / 'os-camera' / 'receiver.ndjson'
        result = evidence / 'os-camera' / 'result.json'
        if not receiver.exists() or not result.exists():
            rows.append(not_judged('OS virtual camera', 'os-camera', 'receiver evidence missing'))
        elif read_json(result).get('exitCode') != 0:
            rows.append(not_judged('OS virtual camera', 'os-camera', 'receiver exited with ' + str(read_json(result).get('exitCode'))))
        else:
            start_us, end_us = start * 1_000_000 // frequency, -(-end * 1_000_000 // frequency)
            script = ("import {readFileSync} from 'node:fs';"
                      "import {judgeCameraPixels} from '" + (HERE / 'camera-pixel-receiver.mjs').as_uri() + "';"
                      "const r=judgeCameraPixels(readFileSync(process.argv[1],'utf8'),{warmupSeconds:0,"
                      "minimumSeconds:Number(process.argv[2]),startHostUs:Number(process.argv[3]),endHostUs:Number(process.argv[4])});"
                      "console.log(JSON.stringify(r));")
            camera, proof = attempt('os-camera.json', lambda: json.loads(subprocess.run(
                [tools['node'], '--input-type=module', '-e', script, str(receiver), str(seconds), str(start_us), str(end_us)],
                capture_output=True, text=True, check=True).stdout))
            window = camera.get('measurement', {}) if isinstance(camera, dict) else {}
            covers = bool(camera.get('coverage', {}).get('complete') and window.get('includesEndpointBrackets')
                          and window.get('startHostUs') == start_us and window.get('endHostUs') == end_us) if isinstance(camera, dict) else False
            rows.append(camera_row(camera, proof, covers, (evidence / 'camera-serving-identity.json').exists()))
            samples = [json.loads(line) for line in receiver.read_text(encoding='utf-8').splitlines() if line.strip()]
            bounds = bounded_camera_identities(samples, start_us, end_us)
            if bounds:
                save('bounded-program-identities.json', {'first': bounds[0], 'last': bounds[1], 'samples': bounds[2],
                     'basis': 'Independent OS receiver arrivals within the declared QPC interval'})
    if bounds is None:
        notes.append('Program recording and stream judged over their whole files: no OS camera interval was available to bound them.')
    first, last = (bounds[0], bounds[1]) if bounds else (None, None)

    program_path = final.get('recording', {}).get('artifactPath')
    if not finalized_ok:
        rows.insert(0, not_judged('Program recording', 'program', 'recording did not finalize'))
    elif not program_path:
        rows.insert(0, recording_row('Program recording', 'program', {'error': 'no Program artifact path in finalized snapshot'}, None, False))
    else:
        verdict, proof = attempt('program.json', lambda: record_module.judge(program_path, ffmpeg, ffprobe, seconds, first, last))
        admissions, _ = attempt('program-source-admission.json', lambda: json.loads(subprocess.run(
            [tools['node'], str(HERE / 'program-source-admission-capture.mjs'), str(evidence / 'samples.ndjson'),
             str(evidence / 'measurement.json'), *drill.get('programSourceIds', [])],
            capture_output=True, text=True, check=True).stdout))
        rows.insert(0, gate_program_admissions(recording_row('Program recording', 'program', verdict, proof, bounds is not None), admissions))

    tracks = {row.get('sourceId'): row for row in final.get('recording', {}).get('streams', []) if row.get('kind') == 'iso'}
    for source in enabled.get('iso', []):
        track = tracks.get(source)
        if not finalized_ok:
            rows.append(not_judged('ISO ' + source, 'iso', 'recording did not finalize', source))
        elif track is None or not track.get('path'):
            rows.append({**not_judged('ISO ' + source, 'iso', 'ISO track missing from the finalized recording', source), 'status': FAIL})
        elif not trace_ok:
            rows.append(not_judged('ISO ' + source, 'iso', 'delivery trace incomplete or missing', source))
        else:
            name = 'iso-' + source.replace(':', '_') + '.json'
            held_frames = int(track.get('heldFrames') or 0)
            verdict, proof = attempt(name, lambda: iso_module.judge(track['path'], str(trace), source, start, end, seconds,
                                                                    ffmpeg, ffprobe, held_frames))
            rows.append(iso_row(source, verdict, proof))

    if enabled.get('stream'):
        received = evidence / 'stream-received.ts'
        if not received.exists() or received.stat().st_size == 0:
            rows.append(not_judged('Stream', 'stream', 'local SRT sink wrote no transport stream'))
        else:
            verdict, proof = attempt('stream.json', lambda: record_module.judge(str(received), ffmpeg, ffprobe, seconds, first, last))
            rows.append(recording_row('Stream', 'stream', verdict, proof, bounds is not None))

    # Supplementary, never part of the overall verdict.
    if trace_ok and drill.get('programSourceIds'):
        delivery_module = load_module('delivery_trace', 'delivery-trace.py')
        deadlines, _ = attempt('program-deadlines.json', lambda: delivery_module.judge(
            trace, 0, seconds, start_ticks=start, end_ticks=end, require_program_deadlines=True,
            require_exact_source_selection=True, expected_source_ids=drill['programSourceIds']))
        context['supplementary']['programGpuDeadlines'] = deadlines.get('result', 'NOT JUDGED') if isinstance(deadlines, dict) else 'NOT JUDGED'
    return aggregate(rows, context)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('evidence', type=pathlib.Path)
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    try:
        verdict = run(evidence)
    except Exception as error:  # never leave a drill without a verdict file
        verdict = aggregate([], {'collectionFailure': f'judge orchestration failed: {error}',
                                 'notes': [traceback.format_exc(limit=3)]})
    (evidence / 'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
    text = summary_text(verdict)
    (evidence / 'summary.txt').write_text(text, encoding='utf-8')
    sys.stdout.write(text)
    return 0 if verdict['overall'] == PASS else 1


if __name__ == '__main__':
    raise SystemExit(main())
