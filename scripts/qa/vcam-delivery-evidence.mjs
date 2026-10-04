import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const counters = ['emitted', 'fresh', 'held', 'slate', 'failed', 'formatMismatch',
  'readFresh', 'unchanged', 'contended', 'unavailable', 'uninitialized', 'invalidHeader'];
const required = [...counters, 'run', 'publicationObserved', 'lastPublication', 'lastSeq',
  'maxIntervalHns', 'programIdentityVerified', 'receiverVerified'];
const correlationCounters = ['correlatedReads', 'uncorrelatedReads', 'unobservedProgramFrames',
  'programEpochChanges', 'programRegressions'];

// Diagnostic only: neither a fresh SHM read nor an MF event acknowledgment is
// proof of a fresh Program frame or downstream receiver presentation.
export function summarizeCameraDelivery(log) {
  const streams = new Map();
  let malformedRecords = 0;
  for (const line of log.split(/\r?\n/)) {
    const version = Number(line.match(/\[vcam-delivery-v([12])\]/)?.[1]);
    if (!version) continue;
    const rowFields = version === 2 ? [...required, ...correlationCounters, 'lastProgramSequence', 'lastReadProgramSequence'] : required;
    const rowCounters = version === 2 ? [...counters, ...correlationCounters] : counters;
    const pid = line.match(/\bpid=(\d+)/)?.[1];
    const fields = Object.fromEntries([...line.matchAll(/\b(\w+)=([^\s]+)/g)].map(m => [m[1], m[2]]));
    if (!pid || !/^(?:0x)?[0-9a-f]+$/i.test(fields.stream ?? '') ||
        rowFields.some(k => !/^\d+$/.test(fields[k] ?? '') || !Number.isSafeInteger(Number(fields[k])))) {
      ++malformedRecords; continue;
    }
    const row = Object.fromEntries(rowFields.map(k => [k, Number(fields[k])]));
    if (row.emitted !== row.fresh + row.held + row.slate || row.publicationObserved > 1 ||
        row.programIdentityVerified > (version === 2 ? 1 : 0) || row.receiverVerified !== 0 ||
        (version === 2 && (!/^[0-9a-f]{32}$/i.test(fields.epoch ?? '') ||
          row.correlatedReads + row.uncorrelatedReads !== row.readFresh ||
          (row.programIdentityVerified === 1 && (!row.correlatedReads || !row.lastProgramSequence || /^0+$/.test(fields.epoch)))))) {
      ++malformedRecords; continue;
    }
    if (version === 2) row.epoch = fields.epoch;
    const key = `${pid}:${fields.stream}:${row.run}:${version}`;
    let stream = streams.get(key);
    if (!stream) {
      stream = { pid: Number(pid), stream: fields.stream, run: row.run, version, records: 0,
        counterResetObserved: false, first: row, last: row };
      streams.set(key, stream);
    }
    if (rowCounters.some(k => row[k] < stream.last[k])) stream.counterResetObserved = true;
    ++stream.records;
    stream.last = row;
  }
  return {
    schema: 'vcam-delivery-summary-v2', available: streams.size > 0, malformedRecords,
    programIdentityVerified: streams.size > 0 && malformedRecords === 0 && [...streams.values()].every(s =>
      s.version === 2 && !s.counterResetObserved && s.last.programIdentityVerified === 1 &&
      s.last.uncorrelatedReads === 0 && s.last.programRegressions === 0),
    receiverVerified: false,
    limitation: 'MF sample emission only. V2 can correlate pixel publications to Program; unobserved identities are not proof of where loss occurred. Receiver presentation is unverified.',
    streams: [...streams.values()].map(s => ({
      ...s, delta: s.records < 2 || s.counterResetObserved ? null :
        Object.fromEntries((s.version === 2 ? [...counters, ...correlationCounters] : counters)
          .map(k => [k, s.last[k] - s.first[k]])),
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
