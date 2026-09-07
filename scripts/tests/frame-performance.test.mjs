import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { validateFramePerformance, readBoundedReport } from '../qa/validate-frame-performance.mjs';

function fixture() {
  const startTime = '2026-09-06T12:00:00.000Z', endTime = '2026-09-06T12:00:02.000Z';
  const requiredPaths = ['cpu-submission', 'program-gpu', 'program-presentation', 'recording:program'];
  return { startTime, endTime, soakPassed: true, errorMatches: [], performanceEvidence: {
    metricVersion: 'anchored-deadline-v1', targetFps: 60, clock: 'monotonic',
    requiredPaths, enabledOutputs: ['recording:program'], paths: requiredPaths.map(id => ({
      id, measurement: 'completion-deadlines', coverage: { complete: true, startTime, endTime },
      firstSlot: 1, lastSlot: 120, expectedSlots: 120, completedSlots: 120,
      uniqueCompletedSlots: 120, deadlineMisses: 0, skippedSlots: 0, errors: []
    }))
  }};
}

test('complete exact evidence passes independently of operator functional result', () => {
  const report = fixture();
  report.soakPassed = false;
  const verdict = validateFramePerformance(report);
  assert.equal(verdict.status, 'passed');
  assert.equal(verdict.framePerformancePassed, true);
  assert.equal(verdict.operatorFunctionalPassed, false);
});

for (const [name, mutate] of [
  ['one deadline miss despite 60 average FPS', r => { r.fps = { median: 60 }; r.performanceEvidence.paths[1].deadlineMisses = 1; }],
  ['one skipped slot', r => r.performanceEvidence.paths[1].skippedSlots = 1],
  ['duplicate completion', r => r.performanceEvidence.paths[2].uniqueCompletedSlots--],
  ['missing completion', r => r.performanceEvidence.paths[2].completedSlots--],
  ['missing slot range', r => r.performanceEvidence.paths[1].lastSlot--],
  ['zero samples', r => r.performanceEvidence.paths[1].expectedSlots = 0],
  ['insufficient samples for duration', r => r.endTime = '2026-09-06T12:30:00.000Z'],
  ['unsafe integer counters', r => r.performanceEvidence.paths[0].completedSlots = Number.MAX_SAFE_INTEGER + 1],
  ['path errors', r => r.performanceEvidence.paths[3].errors.push('encoder failure')],
  ['report errors', r => r.errorMatches.push('exception')],
  ['late exact presentation interval', r => r.performanceEvidence.paths[2].worstPresentationIntervalNs = 17000000],
]) test(name + ' prevents performance acceptance', () => {
  const report = fixture(); mutate(report);
  assert.equal(validateFramePerformance(report).framePerformancePassed, false);
});

for (const [name, mutate] of [
  ['missing GPU evidence', r => r.performanceEvidence.paths.splice(1, 1)],
  ['missing presentation evidence', r => r.performanceEvidence.paths.splice(2, 1)],
  ['missing enabled output', r => r.performanceEvidence.paths.pop()],
  ['unlisted enabled output', r => r.performanceEvidence.enabledOutputs.push('ndi:program')],
  ['missing enabled inventory', r => delete r.performanceEvidence.enabledOutputs],
  ['boundary window missing', r => r.performanceEvidence.paths[0].coverage.complete = false],
  ['first window missing', r => r.performanceEvidence.paths[0].coverage.startTime = r.endTime],
  ['last partial window missing', r => r.performanceEvidence.paths[0].coverage.endTime = r.startTime],
  ['missing error evidence', r => delete r.errorMatches],
  ['CPU submission mislabeled as loop timing', r => r.performanceEvidence.paths[0].measurement = 'loop-start'],
]) test(name + ' reports insufficient evidence', () => {
  const report = fixture(); mutate(report);
  const verdict = validateFramePerformance(report);
  assert.equal(verdict.framePerformancePassed, false);
  assert.equal(verdict.status, 'insufficient-evidence');
});

test('legacy functional pass with perfect averages never proves frame delivery', () => {
  const report = fixture(); delete report.performanceEvidence;
  Object.assign(report, { fps: { count: 900, median: 60, min: 60, max: 60 }, reportedDrops: 0, worstFrameMaxMs: 16.6 });
  assert.deepEqual([validateFramePerformance(report).operatorFunctionalPassed, validateFramePerformance(report).status], [true, 'insufficient-evidence']);
});

test('CPU-only exact counters cannot substitute for GPU or presentation evidence', () => {
  const report = fixture();
  report.performanceEvidence.requiredPaths = ['cpu-submission'];
  report.performanceEvidence.enabledOutputs = [];
  report.performanceEvidence.paths = report.performanceEvidence.paths.slice(0, 1);
  assert.equal(validateFramePerformance(report).status, 'insufficient-evidence');
});

test('presentation interval accepts the integer-nanosecond rational 60 Hz boundary', () => {
  const report = fixture();
  report.performanceEvidence.paths[2].worstPresentationIntervalNs = 16666667;
  assert.equal(validateFramePerformance(report).framePerformancePassed, true);
});

test('producer overruns are diagnostic when every scheduled GPU and output deadline succeeds', () => {
  const report = fixture();
  Object.assign(report.performanceEvidence.paths[0], { completedSlots: 118, uniqueCompletedSlots: 118, deadlineMisses: 9, skippedSlots: 2 });
  report.reportedDrops = 330;
  const verdict = validateFramePerformance(report);
  assert.equal(verdict.framePerformancePassed, true);
  assert.ok(verdict.diagnostics.some(item => item.includes('deadlineMisses=9')));
  assert.ok(verdict.diagnostics.some(item => item.includes('330 late loop intervals')));
});

test('presentation interval rejects one nanosecond beyond the rational 60 Hz boundary', () => {
  const report = fixture();
  report.performanceEvidence.paths[2].worstPresentationIntervalNs = 16666668;
  const verdict = validateFramePerformance(report);
  assert.equal(verdict.framePerformancePassed, false);
  assert.ok(verdict.issues.some(issue => issue.includes('nanosecond-quantized')));
});

test('CLI fails closed, bounds input and preserves original evidence', () => {
  const directory = mkdtempSync(join(tmpdir(), 'frame-performance-'));
  try {
    const source = join(directory, 'report.json'), output = join(directory, 'verdict.json');
    const text = JSON.stringify(fixture()); writeFileSync(source, text);
    const cli = new URL('../qa/validate-frame-performance.mjs', import.meta.url);
    const run = (...args) => spawnSync(process.execPath, [fileURLToPath(cli), ...args], { encoding: 'utf8' });
    assert.equal(run(source, output).status, 0);
    assert.equal(JSON.parse(readFileSync(output)).framePerformancePassed, true);
    assert.equal(run(source, source).status, 1);
    assert.equal(readFileSync(source, 'utf8'), text);
    writeFileSync(source, '{}'); assert.equal(run(source).status, 1);
    writeFileSync(source, 'invalid JSON'); assert.equal(run(source).status, 1);
    writeFileSync(source, ' '.repeat(4 * 1024 * 1024 + 1));
    assert.throws(() => readBoundedReport(source), /bounded evidence/);
  } finally { rmSync(directory, { recursive: true, force: true }); }
});
