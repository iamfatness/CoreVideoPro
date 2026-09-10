// scripts/qa/take-verdict-judge.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeTakeRecords, scopeTakeRecords } from './take-verdict-judge.mjs';

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

// scopeTakeRecords — excludes the harness's own setup record, records from scenes
// outside the harness's pair, pre-phase records, and a post-phase restore back to a
// third scene; keeps only records genuinely armed by the harness's own Takes loop.
test('scopeTakeRecords excludes the soak setup record (unloaded -> pgm)', () => {
  const records = [
    { fromSceneId: 'unloaded', toSceneId: 'pgm', armedAtMs: 100, verdict: 'cut' },
    { fromSceneId: 'take-a', toSceneId: 'take-b', armedAtMs: 200, verdict: 'cut' },
  ];
  const scoped = scopeTakeRecords(records, { sceneA: 'take-a', sceneB: 'take-b', armedAfterMs: 0 });
  assert.equal(scoped.length, 1);
  assert.equal(scoped[0].armedAtMs, 200);
});

test('scopeTakeRecords excludes a record from a scene outside the harness pair', () => {
  const records = [
    { fromSceneId: 'take-a', toSceneId: 'solo-b', armedAtMs: 500, verdict: 'cut' },
  ];
  const scoped = scopeTakeRecords(records, { sceneA: 'take-a', sceneB: 'take-b', armedAfterMs: 0 });
  assert.equal(scoped.length, 0);
});

test('scopeTakeRecords excludes a pre-phase record even when both scenes match', () => {
  const records = [
    { fromSceneId: 'take-a', toSceneId: 'take-b', armedAtMs: 100, verdict: 'cut' },
  ];
  const scoped = scopeTakeRecords(records, { sceneA: 'take-a', sceneB: 'take-b', armedAfterMs: 150 });
  assert.equal(scoped.length, 0);
});

test('scopeTakeRecords keeps in-phase records', () => {
  const records = [
    { fromSceneId: 'take-a', toSceneId: 'take-b', armedAtMs: 200, verdict: 'cut' },
    { fromSceneId: 'take-b', toSceneId: 'take-a', armedAtMs: 300, verdict: 'cut' },
  ];
  const scoped = scopeTakeRecords(records, { sceneA: 'take-a', sceneB: 'take-b', armedAfterMs: 150 });
  assert.equal(scoped.length, 2);
});

test('scopeTakeRecords excludes the post-phase restore back to the original scene', () => {
  const records = [
    { fromSceneId: 'take-a', toSceneId: 'take-b', armedAtMs: 200, verdict: 'cut' },
    { fromSceneId: 'take-b', toSceneId: 'pgm', armedAtMs: 900, verdict: 'cut' },
  ];
  const scoped = scopeTakeRecords(records, { sceneA: 'take-a', sceneB: 'take-b', armedAfterMs: 150 });
  assert.equal(scoped.length, 1);
  assert.equal(scoped[0].toSceneId, 'take-b');
});
