import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeCameraPixels } from './camera-pixel-receiver.mjs';
const fixture = () => [
  { schema: 'camera-pixel-receiver-v1', width: 1920, height: 1080, fpsNumerator: 60, fpsDenominator: 1 },
  ...Array.from({ length: 121 }, (_, i) => ({ sample: i + 1, arrivalUs: Math.round(i * 1e6 / 60), pts100ns: Math.round(i * 1e7 / 60), identity: i + 500 })),
  { complete: true, samples: 121 }
];
const judge = rows => judgeCameraPixels(rows.map(JSON.stringify).join('\n'), { warmupSeconds: 0, minimumSeconds: 2 });
test('accepts a complete rational 60/1 identity sequence', () => assert.equal(judge(fixture()).receiverPixelContinuityPassed, true));

test('a complete sequence cannot conceal frame acquisition failures', () => {
  for (const frameFailures of [1, -1, null, '0']) {
    const rows = fixture(); rows.at(-1).frameFailures = frameFailures;
    assert.equal(judge(rows).receiverPixelContinuityPassed, false);
  }
  const rows = fixture(); rows.at(-1).frameFailures = 0;
  assert.equal(judge(rows).receiverPixelContinuityPassed, true);
});

test('coverage comes from actual receiver arrivals after warmup, not capture duration alone', () => {
  const rows = fixture();
  assert.equal(judge(rows).coverage.complete, false);
  rows[0].startUnixUs = Date.parse('2026-10-16T12:00:00Z') * 1000;
  const verdict = judgeCameraPixels(rows.map(JSON.stringify).join('\n'), { warmupSeconds: 1, minimumSeconds: 1 });
  assert.equal(verdict.firstIdentity, 560);
  assert.equal(verdict.lastIdentity, 620);
  assert.deepEqual(verdict.coverage, { complete: true,
    startTime: '2026-10-16T12:00:01.000Z', endTime: '2026-10-16T12:00:02.000Z' });
  rows.pop();
  assert.equal(judge(rows).coverage.complete, false);
});
test('direct DLL evidence never certifies the registered OS camera', () => {
  const rows = fixture(); rows[0].receiverMode = 'direct-dll';
  assert.equal(judge(rows).receiverPixelContinuityPassed, true);
  assert.equal(judge(rows).osCameraContinuityVerified, false);
  rows[0].receiverMode = 'unknown';
  assert.equal(judge(rows).receiverPixelContinuityPassed, false);
});
test('rejects repeated identities even at a perfect 60fps arrival rate', () => {
  const rows = fixture(); rows[50].identity = rows[49].identity;
  assert.equal(judge(rows).receiverPixelContinuityPassed, false); assert.equal(judge(rows).repeated, 1);
});
test('multimedia scheduling does not waive pixel loss or a late receiver interval', () => {
  for (const failure of ['identity', 'arrival']) {
    const rows = fixture(); rows[0].receiverScheduling = 'Playback';
    if (failure === 'identity') rows[50].identity = rows[49].identity;
    else rows[50].arrivalUs += 20000;
    const verdict = judge(rows);
    assert.equal(verdict.receiverScheduling, 'Playback');
    assert.equal(verdict.receiverPixelContinuityPassed, false);
  }
});
test('rejects missing, torn and reordered identities', () => {
  for (const value of [null, 1, 900]) { const rows = fixture(); rows[50].identity = value; assert.equal(judge(rows).receiverPixelContinuityPassed, false); }
});
test('rejects partial files and resets', () => {
  assert.equal(judge(fixture().slice(0, -1)).receiverPixelContinuityPassed, false);
  const rows = fixture(); rows[50].sample = 1; assert.equal(judge(rows).receiverPixelContinuityPassed, false);
});
test('does not certify another negotiated rate or a short run', () => {
  const rows = fixture(); rows[0].fpsDenominator = 1001; assert.equal(judge(rows).receiverPixelContinuityPassed, false);
  assert.equal(judgeCameraPixels(fixture().map(JSON.stringify).join('\n')).receiverPixelContinuityPassed, false);
});
test('rejects continuous identities delivered at 50fps despite a declared 60/1 type', () => {
  const rows = fixture();
  for (let index = 1; index < rows.length - 1; ++index) rows[index].arrivalUs = (index - 1) * 20000;
  assert.equal(judge(rows).receiverPixelContinuityPassed, false);
});
const timedFixture = () => fixture().map(row => Number.isSafeInteger(row.sample)
  ? { ...row, arrivalHostUs: 1_000_000 + row.arrivalUs } : row);
const judgeWindow = (rows, overrides = {}) => judgeCameraPixels(rows.map(JSON.stringify).join('\n'),
  { startHostUs: 1_505_000, endHostUs: 2_505_000, minimumSeconds: 1, ...overrides });
test('judges the predeclared host-clock interval with samples bracketing both endpoints', () => {
  const rows = timedFixture(); rows[10].identity = rows[9].identity;
  assert.equal(judge(rows).receiverPixelContinuityPassed, false); // retain whole-capture failure
  const report = judgeWindow(rows);
  assert.equal(report.receiverPixelContinuityPassed, true);
  assert.equal(report.firstIdentity, 530); assert.equal(report.lastIdentity, 591);
  assert.equal(report.measurement.includesEndpointBrackets, true);
});
test('a repeated frame at the declared interval boundary or interior still fails', () => {
  for (const index of [32, 50, 92]) {
    const rows = timedFixture(); rows[index].identity = rows[index - 1].identity;
    assert.equal(judgeWindow(rows).receiverPixelContinuityPassed, false);
  }
});
test('refuses short, absent, reordered or uncovered host-clock evidence', () => {
  for (const options of [{ startHostUs: -1 }, { endHostUs: 1_600_000 }, { endHostUs: 4_000_000 }, { startHostUs: undefined }])
    assert.equal(judgeWindow(timedFixture(), options).receiverPixelContinuityPassed, false);
  assert.equal(judgeWindow(fixture()).receiverPixelContinuityPassed, false);
  const rows = timedFixture(); rows[50].arrivalHostUs = rows[49].arrivalHostUs;
  assert.equal(judgeWindow(rows).receiverPixelContinuityPassed, false);
});
