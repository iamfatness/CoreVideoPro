import test from 'node:test';
import assert from 'node:assert/strict';
import { summarizePublicationExperiment } from './publication-qa-evidence.mjs';
const lines = rows => rows.map(row => JSON.stringify(row)).join('\n');
function fixture(oddUs = 8000) {
  const rows = Array.from({ length: 60 }, (_, index) => {
    const dueUs = 1000000 + Math.floor(index * 1000000 / 60);
    return { identity: index + 1, dueUs, beginUs: dueUs, endUs: dueUs + oddUs + 100 };
  });
  return { publisher: [{ schema: 'camera-publication-qa-v1', fps: 60, seconds: 1, oddUs, isolated: true }, ...rows, { complete: true, published: 60 }],
    receiver: [{ schema: 'camera-pixel-receiver-v1' }, ...rows.map((row, index) => ({ sample: index + 1, identity: row.identity, arrivalUs: index * 16667, arrivalHostUs: row.endUs + 500 })), { complete: true, samples: 60 }] };
}
test('reports actual fault duration and cross-process pixel age', () => {
  const { publisher, receiver } = fixture();
  const result = summarizePublicationExperiment(lines(publisher), lines(receiver), 0);
  assert.equal(result.evidenceValid, true);
  assert.equal(result.oddWindowMinimumMs, 8.1);
  assert.equal(result.publicationToReceiverMedianMs, .5);
  assert.equal(result.publisherDeadlineOverruns, 0);
});
test('reports over-deadline source faults without concealing them', () => {
  const { publisher, receiver } = fixture(20000);
  assert.equal(summarizePublicationExperiment(lines(publisher), lines(receiver), 0).publisherDeadlineOverruns, 60);
});
test('rejects missing clocks, source identities and inconsistent clocks', () => {
  for (const mutation of [row => delete row.arrivalHostUs, row => row.identity = 900, row => row.arrivalHostUs = 1]) {
    const { publisher, receiver } = fixture(); mutation(receiver[1]);
    assert.equal(summarizePublicationExperiment(lines(publisher), lines(receiver), 0).evidenceValid, false);
  }
});
test('rejects truncated receiver and publisher captures and weakened injection', () => {
  for (const mutation of [(p, r) => r.pop(), (p, r) => p.pop(), (p, r) => p[1].endUs = p[1].beginUs]) {
    const { publisher, receiver } = fixture(); mutation(publisher, receiver);
    assert.equal(summarizePublicationExperiment(lines(publisher), lines(receiver), 0).evidenceValid, false);
  }
});
