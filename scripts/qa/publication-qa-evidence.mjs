import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const percentile = (values, fraction) => values.length ? [...values].sort((a, b) => a - b)[Math.ceil(fraction * values.length) - 1] : null;
export function summarizePublicationExperiment(publisherText, receiverText, warmupSeconds = 30) {
  const publisher = publisherText.trim().split(/\r?\n/).map(line => JSON.parse(line));
  const receiver = receiverText.trim().split(/\r?\n/).map(line => JSON.parse(line));
  const header = publisher[0], complete = publisher.at(-1);
  const rows = publisher.slice(1, -1);
  const errors = [];
  const received = receiver.filter(row => Number.isSafeInteger(row.sample));
  if (receiver[0]?.schema !== 'camera-pixel-receiver-v1' || receiver.at(-1)?.complete !== true ||
      receiver.at(-1)?.samples !== received.length) errors.push('Incomplete receiver evidence.');
  if (header?.schema !== 'camera-publication-qa-v1' || header.fps !== 60 || header.isolated !== true ||
      !Number.isInteger(header.seconds) || header.seconds < 1 || header.seconds > 180 ||
      !Number.isInteger(header.oddUs) || header.oddUs < 0 || header.oddUs > 25000 ||
      complete?.complete !== true || complete.published !== rows.length || rows.length !== header.seconds * 60)
    errors.push('Missing or incomplete isolated publisher evidence.');
  for (let index = 0; index < rows.length; ++index) {
    const row = rows[index];
    if (row.identity !== index + 1 || ![row.dueUs, row.beginUs, row.endUs].every(Number.isSafeInteger) ||
        row.beginUs < row.dueUs || row.endUs < row.beginUs || row.endUs - row.beginUs < header.oddUs ||
        (index && Math.abs(row.dueUs - rows[index - 1].dueUs - 1000000 / 60) > 1)) {
      errors.push('Malformed publication identity, cadence or timestamps.'); break;
    }
  }
  const observed = receiver.filter(row => Number.isSafeInteger(row.sample) && row.arrivalUs >= warmupSeconds * 1000000);
  if (!observed.length) errors.push('No measured receiver samples.');
  const byIdentity = new Map(rows.map(row => [row.identity, row]));
  const ages = [];
  for (const sample of observed) {
    const publication = byIdentity.get(sample.identity);
    if (sample.identity === null) continue; // slates/invalid pixels are scored by the receiver judge
    if (!publication || !Number.isSafeInteger(sample.arrivalHostUs)) {
      errors.push('Missing cross-process timestamp or publisher identity.'); continue;
    }
    const age = sample.arrivalHostUs - publication.endUs;
    if (age < 0) errors.push('Receiver precedes publication; clocks/evidence inconsistent.');
    else ages.push(age / 1000);
  }
  const oddWindows = rows.map(row => (row.endUs - row.beginUs) / 1000);
  const deadlineOverruns = rows.filter(row => row.endUs > row.dueUs + 1000000 / 60).length;
  const valid = observed.filter(row => byIdentity.has(row.identity));
  const measuredSource = valid.length ? rows.filter(row => row.identity >= valid[0].identity && row.identity <= valid.at(-1).identity) : [];
  return {
    schema: 'publication-experiment-summary-v1', evidenceValid: errors.length === 0,
    injectedOddUs: header?.oddUs, published: rows.length, publisherDeadlineOverruns: deadlineOverruns,
    measuredPublisherDeadlineOverruns: measuredSource.length ? measuredSource.filter(row => row.endUs > row.dueUs + 1000000 / 60).length : null,
    oddWindowMinimumMs: oddWindows.length ? Math.min(...oddWindows) : null,
    oddWindowP99Ms: percentile(oddWindows, .99), oddWindowMaximumMs: oddWindows.length ? Math.max(...oddWindows) : null,
    measuredReceiverSamples: observed.length, correlatedPixelAges: ages.length,
    publicationToReceiverMedianMs: percentile(ages, .5), publicationToReceiverP95Ms: percentile(ages, .95),
    errors: [...new Set(errors)],
    limitation: 'Diagnostic publisher and actual receiver timestamps only. Judge pixel continuity separately. Publication-to-receiver age does not measure capture-to-display latency or A/V sync. A publication beyond its next deadline invalidates an assumption of fault-free source cadence.'
  };
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 4) throw new Error('Usage: publication-qa-evidence.mjs PUBLISHER.ndjson RECEIVER.ndjson');
    const report = summarizePublicationExperiment(readFileSync(process.argv[2], 'utf8'), readFileSync(process.argv[3], 'utf8'));
    console.log(JSON.stringify(report, null, 2)); process.exitCode = report.evidenceValid ? 0 : 1;
  } catch (error) { console.error(error.message); process.exitCode = 2; }
}
