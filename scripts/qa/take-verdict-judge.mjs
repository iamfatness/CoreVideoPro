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
