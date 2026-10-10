"""Read-only trace of native lower-third content and matching Zoom metadata."""
import argparse
import collections
import datetime
import json
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=float, default=45)
parser.add_argument('--out', required=True)
args = parser.parse_args()
counts = collections.Counter()
transitions = []
last = None
samples = 0
start = time.monotonic()
with open(args.out, 'w', encoding='utf-8') as file:
    while time.monotonic() - start < args.seconds:
        envelope = json.load(urllib.request.urlopen('http://127.0.0.1:8011/snapshot', timeout=5))
        if not envelope.get('available') or envelope.get('stale'):
            raise RuntimeError('Native observation unavailable or stale')
        snapshot = envelope['snapshot']
        overlays = snapshot.get('overlayState', {}).get('overlays', [])
        key = next((o for o in overlays if o.get('overlayId') == 'key:lower-third'), None)
        participants = snapshot.get('participants', [])
        source_id = key.get('sourceId', '') if key else ''
        participant = next((p for p in participants if 'zoom:' + str(p.get('userId')) == source_id), {})
        row = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'receivedUtc': envelope.get('receivedUtc'), 'rosterRevision': snapshot.get('rosterRevision'),
               'key': key, 'participant': {field: participant.get(field) for field in ('userId', 'role', 'title', 'breakoutRoomName')},
               'program': snapshot.get('programFrame', {}).get('videoSources', [])}
        file.write(json.dumps(row, ensure_ascii=False) + '\n')
        counts[key.get('title') if key else '<missing>'] += 1
        signature = tuple(key.get(field) for field in ('sourceId', 'sourceName', 'title', 'org', 'keyPhase')) if key else None
        if signature != last:
            transitions.append(row)
            last = signature
        samples += 1
        time.sleep(.2)
summary = {'seconds': time.monotonic() - start, 'samples': samples,
           'titleCounts': dict(counts), 'transitions': transitions,
           'scope': 'Native snapshots; sampled metadata and key state, not every output frame'}
with open(args.out + '.summary.json', 'w', encoding='utf-8') as file:
    json.dump(summary, file, indent=2, ensure_ascii=False)
print(json.dumps({'samples': samples, 'titleCounts': dict(counts), 'transitions': len(transitions)}))
