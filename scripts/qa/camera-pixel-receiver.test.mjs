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
