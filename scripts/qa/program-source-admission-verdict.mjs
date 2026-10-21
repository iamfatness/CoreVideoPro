// Read-only source evidence. Output sequence markers cannot prove source images.
const natural = n => Number.isSafeInteger(n) && n >= 0;
const canonical = id => typeof id === 'string' ? id.replace(/^zoom:/, '') : '';

export function assessProgramSourceAdmissions(snapshot, expectedSourceIds) {
  const issues = [], missingEvidence = [];
  const observed = snapshot?.programFrame?.videoSources;
  const expected = expectedSourceIds ?? (Array.isArray(observed) ? observed.map(s => s.sourceId) : null);
  if (!Array.isArray(expected) || expected.length > 64 || expected.some(id => !canonical(id))) {
    return { sourceAdmissionsVerified: false, issues, missingEvidence: ['Explicit bounded Program source identities required'] };
  }
  if (!expected.length) return { sourceAdmissionsVerified: true, issues, missingEvidence };
  const proof = snapshot?.programSourceAdmission;
  if (proof?.version !== 1 || !Array.isArray(proof.sources) || proof.sources.length > 64) {
    return { sourceAdmissionsVerified: false, issues, missingEvidence: ['Program source admission evidence required independently of output counters'] };
  }
  for (const id of expected) {
    const rows = proof.sources.filter(s => canonical(s?.sourceId) === canonical(id));
    if (rows.length !== 1) {
      missingEvidence.push(`Exactly one admission required for ${id}`);
      continue;
    }
    const s = rows[0];
    if (s.state !== 'ready') issues.push({ sourceId: id, reason: 'Program source image not ready', state: s.state });
    if (!natural(s.actualEpoch) || !s.actualEpoch || !natural(s.actualFrameId) ||
        !natural(s.actualCapture100ns) || !s.actualCapture100ns) {
      issues.push({ sourceId: id, reason: 'Actual Program source identity absent' });
    } else if (s.actualEpoch !== s.requestedEpoch || s.actualFrameId !== s.requestedFrameId ||
               s.actualCapture100ns !== s.requestedCapture100ns) {
      issues.push({ sourceId: id, reason: 'Actual Program image differs from selected source identity' });
    }
  }
  return { sourceAdmissionsVerified: !issues.length && !missingEvidence.length, issues, missingEvidence };
}
