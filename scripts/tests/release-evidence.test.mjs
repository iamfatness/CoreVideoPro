import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createCandidate, requiredChecks, sha256, validateEvidence } from '../release-evidence.mjs';

function fixture() {
  const artifactBytes = Buffer.from('signed candidate'), report = Buffer.from('sanitized rig report');
  const configuration = { platform: 'windows-x64', flags: { COREVIDEO_STUB: 'OFF' }, runtimeFiles: { 'sdk.dll': sha256('sdk') } };
  const candidate = { schemaVersion: 1, sourceSha: 'a'.repeat(40), createdAt: '2026-09-01T00:00:00Z', configuration,
    configurationSha256: sha256(JSON.stringify(configuration)), artifact: { name: 'CoreVideoPro.msix', sha256: sha256(artifactBytes) } };
  const evidence = { schemaVersion: 1, sourceSha: candidate.sourceSha, configurationSha256: candidate.configurationSha256,
    artifactSha256: candidate.artifact.sha256, startedAt: '2026-09-01T01:00:00Z', completedAt: '2026-09-01T04:00:00Z',
    environment: Object.fromEntries(['rigId', 'os', 'gpu', 'driver', 'zoomSdkVersion', 'runtimeVersions', 'baselineId'].map(k => [k, 'fixture'])),
    checks: requiredChecks.map(id => ({ id, status: 'passed', attachments: [{ path: 'report.txt', sha256: sha256(report) }] })) };
  Object.assign(evidence.checks.find(c => c.id === 'simultaneous-soak'), { durationSeconds: 7200,
    metrics: Object.fromEntries(['memorySlopeMbPerHour', 'frameDropPercent', 'queueDepth', 'commandP95Ms', 'commandMaxMs', 'avDriftMs']
      .map(k => [k, { value: 0, maximum: 1 }])) });
  return { candidate, evidence, options: { artifactBytes, readAttachment: () => report, now: new Date('2026-09-01T05:00:00Z') } };
}
test('accepts complete exact-candidate hardware evidence', () => {
  const f = fixture(); assert.equal(validateEvidence(f.candidate, f.evidence, f.options).status, 'passed');
});
for (const [name, mutate] of Object.entries({
  'wrong commit': f => f.evidence.sourceSha = 'b'.repeat(40),
  'wrong configuration': f => f.evidence.configurationSha256 = 'b'.repeat(64),
  'tampered configuration': f => f.candidate.configuration.flags.COREVIDEO_STUB = 'ON',
  'wrong artifact evidence': f => f.evidence.artifactSha256 = 'b'.repeat(64),
  'different artifact bytes': f => f.options.artifactBytes = Buffer.from('rebuild'),
  'stale evidence': f => f.options.now = new Date('2026-10-01T00:00:00Z'),
  'evidence predates candidate': f => f.evidence.startedAt = '2026-08-31T23:00:00Z',
  'future evidence': f => f.evidence.completedAt = '2026-09-02T00:00:00Z',
  'missing check': f => f.evidence.checks.pop(),
  'failed check': f => f.evidence.checks[0].status = 'failed',
  'skipped check': f => f.evidence.checks[0].status = 'skipped',
  'duplicate check': f => f.evidence.checks.push(f.evidence.checks[0]),
  'missing environment': f => delete f.evidence.environment.driver,
  'missing attachment': f => f.evidence.checks[0].attachments = [],
  'tampered attachment': f => f.options.readAttachment = () => Buffer.from('wrong'),
  'short soak': f => f.evidence.checks[2].durationSeconds = 60,
  'missing metric': f => delete f.evidence.checks[2].metrics.avDriftMs,
  'threshold exceeded': f => f.evidence.checks[2].metrics.avDriftMs.value = 2,
  'latency threshold weakened': f => f.evidence.checks[2].metrics.commandP95Ms.maximum = 251,
})) test(`rejects ${name}`, () => {
  const f = fixture(); mutate(f); assert.throws(() => validateEvidence(f.candidate, f.evidence, f.options));
});

test('candidate inventories actual production flags, runtime and signed package', () => {
  const dir = mkdtempSync(join(tmpdir(), 'corevideo-candidate-'));
  try {
    const payload = join(dir, 'payload'); mkdirSync(payload);
    const artifact = join(dir, 'CoreVideoPro.msix'), cache = join(dir, 'CMakeCache.txt');
    writeFileSync(artifact, 'signed bytes'); writeFileSync(join(payload, 'sdk.dll'), 'sdk bytes');
    writeFileSync(cache, 'COREVIDEO_STUB:BOOL=OFF\nCOREVIDEO_WITH_ZOOM:BOOL=ON\nPRIVATE_SDK_PATH:PATH=C:/private\n');
    const candidate = createCandidate({ sourceSha: 'a'.repeat(40), artifact, cache, payload });
    assert.equal(candidate.artifact.sha256, sha256('signed bytes'));
    assert.equal(candidate.configuration.runtimeFiles['sdk.dll'], sha256('sdk bytes'));
    assert.deepEqual(candidate.configuration.flags, { COREVIDEO_STUB: 'OFF', COREVIDEO_WITH_ZOOM: 'ON' });
    writeFileSync(cache, 'COREVIDEO_STUB:BOOL=ON\nCOREVIDEO_WITH_ZOOM:BOOL=ON\n');
    assert.throws(() => createCandidate({ sourceSha: 'a'.repeat(40), artifact, cache, payload }), /non-stub/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('CLI fails closed on missing evidence and attachment path escape', () => {
  const dir = mkdtempSync(join(tmpdir(), 'corevideo-evidence-'));
  try {
    const f = fixture(), manifest = join(dir, 'candidate.json'), evidencePath = join(dir, 'evidence.json');
    const artifact = join(dir, 'CoreVideoPro.msix'), verdict = join(dir, 'verdict.json');
    const cli = fileURLToPath(new URL('../release-evidence.mjs', import.meta.url));
    const run = () => spawnSync(process.execPath, [cli, 'validate', manifest, evidencePath, artifact, verdict], { encoding: 'utf8' });
    writeFileSync(manifest, JSON.stringify(f.candidate)); writeFileSync(artifact, f.options.artifactBytes);
    assert.equal(run().status, 1);
    // Fresh dates keep this fixture independent of the wall clock.
    const now = Date.now();
    f.candidate.createdAt = new Date(now - 4 * 3600000).toISOString();
    f.evidence.startedAt = new Date(now - 3 * 3600000).toISOString();
    f.evidence.completedAt = new Date(now - 1000).toISOString();
    writeFileSync(manifest, JSON.stringify(f.candidate));
    writeFileSync(join(dir, 'report.txt'), 'sanitized rig report');
    writeFileSync(evidencePath, JSON.stringify(f.evidence));
    assert.equal(run().status, 0);
    assert.equal(JSON.parse(readFileSync(verdict)).status, 'passed');
    f.evidence.checks[0].attachments[0].path = '../outside.txt';
    writeFileSync(evidencePath, JSON.stringify(f.evidence));
    const rejected = run(); assert.equal(rejected.status, 1); assert.match(rejected.stderr, /within evidence directory/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});
