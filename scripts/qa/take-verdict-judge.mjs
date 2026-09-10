// scripts/qa/take-verdict-judge.mjs
//
// Pure judge over the core's per-Take records (`sessionState().takeRecords.records[]`,
// CLAUDE.md "A Take is traceable"). It answers exactly one question: did every completed
// Take cut cleanly, never whether the soak that produced the records behaved.
export function judgeTakeRecords(records, { expectedTakes = 0 } = {}) {
  const list = Array.isArray(records) ? records : [];
  const reasons = [];
  let cuts = 0, rebuilt = 0;
  for (const r of list) {
    if (r.verdict === 'rebuilt') {
      rebuilt++;
      reasons.push({
        fromSceneId: r.fromSceneId, toSceneId: r.toSceneId, verdict: r.verdict,
        restartedSources: r.restartedSources ?? [],
        missingSources: r.missingSources ?? [],
        backgroundDropped: Boolean(r.backgroundDropped),
        subscriptionsChurned: Boolean(r.subscriptionsChurned),
      });
    } else if (r.verdict === 'cut') {
      cuts++;
    }
  }
  const ok = rebuilt === 0 && list.length >= expectedTakes;
  return { ok, cuts, rebuilt, total: list.length, expectedTakes, reasons };
}

/**
 * Scope a raw takeRecords list down to the ones a harness's own Takes phase actually
 * armed. Without this, any record already sitting in the core's ring before the
 * harness's first Take (e.g. the soak's own initial scene setup) — or armed by a
 * post-phase restore back to the original scene — silently counts toward `total`,
 * which can mask a genuinely lost take behind a coincidentally-matching count.
 *
 * A record is kept only if BOTH `fromSceneId` and `toSceneId` are in the harness's
 * own {sceneA, sceneB} pair (this alone excludes a restore back to a THIRD scene,
 * e.g. sceneB -> "pgm") AND its `armedAtMs` is strictly after `armedAfterMs` (the
 * floor read from the ring immediately before the harness's first Take).
 *
 * The pair rule means the harness's FIRST take must already start inside the pair: a
 * take from the setup scene (e.g. pgm -> sceneA) is excluded, so a harness that does not
 * first prime Program to one of the pair's scenes scores N-1 records for N takes and can
 * never pass. live-meeting-soak.mjs primes Program to sceneB before reading the floor.
 */
export function scopeTakeRecords(records, { sceneA, sceneB, armedAfterMs = 0 } = {}) {
  const list = Array.isArray(records) ? records : [];
  const scenes = new Set([sceneA, sceneB]);
  return list.filter((r) => {
    if (!scenes.has(r.fromSceneId) || !scenes.has(r.toSceneId)) return false;
    const armedAtMs = Number(r.armedAtMs);
    if (!Number.isFinite(armedAtMs)) return false;
    return armedAtMs > armedAfterMs;
  });
}
