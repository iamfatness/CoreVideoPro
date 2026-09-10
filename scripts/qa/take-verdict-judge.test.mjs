// scripts/qa/take-verdict-judge.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeTakeRecords } from './take-verdict-judge.mjs';

test('every completed take must be a cut', () => {
  const result = judgeTakeRecords([
    { fromSceneId: 'a', toSceneId: 'b', verdict: 'cut', restartedSources: [], backgroundDropped: false, subscriptionsChurned: false },
    { fromSceneId: 'b', toSceneId: 'a', verdict: 'rebuilt', restartedSources: ['background:bg'], backgroundDropped: false, subscriptionsChurned: false },
  ], { expectedTakes: 2 });
  assert.equal(result.ok, false);
  assert.equal(result.cuts, 1);
  assert.equal(result.rebuilt, 1);
  assert.deepEqual(result.reasons[0].restartedSources, ['background:bg']);
});

test('fewer records than takes is a failure, never a quiet pass', () => {
  const result = judgeTakeRecords([{ verdict: 'cut', restartedSources: [] }], { expectedTakes: 3 });
  assert.equal(result.ok, false);
});

test('a no-wall take with nothing shared is not a failure', () => {
  const result = judgeTakeRecords([{ verdict: 'no-wall', restartedSources: [] }], { expectedTakes: 1 });
  assert.equal(result.ok, true);
});

test('a rebuilt reason carries missingSources', () => {
  const result = judgeTakeRecords([
    { fromSceneId: 'take-a', toSceneId: 'take-b', verdict: 'rebuilt',
      restartedSources: ['background:bg'], missingSources: ['zoom:123'],
      backgroundDropped: false, subscriptionsChurned: false },
  ], { expectedTakes: 1 });
  assert.equal(result.ok, false);
  assert.deepEqual(result.reasons[0].missingSources, ['zoom:123']);
});

test('a rebuilt reason with no missingSources field defaults to an empty array', () => {
  const result = judgeTakeRecords([
    { fromSceneId: 'take-a', toSceneId: 'take-b', verdict: 'rebuilt',
      restartedSources: [], backgroundDropped: false, subscriptionsChurned: false },
  ], { expectedTakes: 1 });
  assert.deepEqual(result.reasons[0].missingSources, []);
});
