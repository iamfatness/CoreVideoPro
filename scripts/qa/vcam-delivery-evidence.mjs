import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const counters = ['emitted', 'fresh', 'held', 'slate', 'failed', 'formatMismatch',
  'readFresh', 'unchanged', 'contended', 'unavailable', 'uninitialized', 'invalidHeader'];
const required = [...counters, 'run', 'publicationObserved', 'lastPublication', 'lastSeq',
  'maxIntervalHns', 'programIdentityVerified', 'receiverVerified'];

// Diagnostic only: neither a fresh SHM read nor an MF event acknowledgment is
// proof of a fresh Program frame or downstream receiver presentation.
export function summarizeCameraDelivery(log) {
  const streams = new Map();
  let malformedRecords = 0;
  for (const line of log.split(/\r?\n/)) {
    if (!line.includes('[vcam-delivery-v1]')) continue;
    const pid = line.match(/\bpid=(\d+)/)?.[1];
    const fields = Object.fromEntries([...line.matchAll(/\b(\w+)=([^\s]+)/g)].map(m => [m[1], m[2]]));
    if (!pid || !/^(?:0x)?[0-9a-f]+$/i.test(fields.stream ?? '') ||
        required.some(k => !/^\d+$/.test(fields[k] ?? '') || !Number.isSafeInteger(Number(fields[k])))) {
      ++malformedRecords; continue;
    }
    const row = Object.fromEntries(required.map(k => [k, Number(fields[k])]));
    if (row.emitted !== row.fresh + row.held + row.slate || row.publicationObserved > 1 ||
        row.programIdentityVerified !== 0 || row.receiverVerified !== 0) {
      ++malformedRecords; continue;
    }
    const key = `${pid}:${fields.stream}:${row.run}`;
    let stream = streams.get(key);
    if (!stream) {
      stream = { pid: Number(pid), stream: fields.stream, run: row.run, records: 0,
        counterResetObserved: false, first: row, last: row };
      streams.set(key, stream);
    }
    if (counters.some(k => row[k] < stream.last[k])) stream.counterResetObserved = true;
    ++stream.records;
    stream.last = row;
  }
  return {
    schema: 'vcam-delivery-summary-v1', available: streams.size > 0, malformedRecords,
    programIdentityVerified: false, receiverVerified: false,
    limitation: 'MF sample emission only. Publication IDs are not Program IDs. Gaps and repeats require receiver correlation.',
    streams: [...streams.values()].map(s => ({
      ...s, delta: s.records < 2 || s.counterResetObserved ? null :
        Object.fromEntries(counters.map(k => [k, s.last[k] - s.first[k]])),
      // This is a lifetime maximum, not a maximum for the sampled delta window.
      lifetimeMaximumEmissionIntervalMs: s.last.maxIntervalHns / 10000,
    })),
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--log') {
    console.error('Usage: node scripts/qa/vcam-delivery-evidence.mjs --log <vcam-serve.log>');
    process.exitCode = 2;
  } else {
    try {
      const report = summarizeCameraDelivery(readFileSync(args[1], 'utf8'));
      console.log(JSON.stringify(report, null, 2));
      if (!report.available || report.malformedRecords > 0 || report.streams.some(s => s.counterResetObserved))
        process.exitCode = 2;
    } catch (error) {
      console.error(error.message);
      process.exitCode = 2;
    }
  }
}
