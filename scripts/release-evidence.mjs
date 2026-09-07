import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { basename, dirname, join, relative, resolve, sep } from 'node:path';
import { pathToFileURL } from 'node:url';

export const requiredChecks = [
  'zoom-multiparticipant-ingest', 'program-iso-record-stream', 'simultaneous-soak',
  'network-interruption-per-destination', 'camera-unplug-replug', 'core-process-termination',
  'zoom-process-termination', 'disk-exhaustion', 'shutdown-during-finalization',
  'decode-and-av-alignment', 'clean-machine-install-update-launch',
];
export const sha256 = data => createHash('sha256').update(data).digest('hex');
const requireThat = (condition, message) => { if (!condition) throw new Error(message); };
const nonempty = value => typeof value === 'string' && value.trim().length > 0;
const hashPattern = /^[a-f0-9]{64}$/;
const readJson = file => JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));

// Constructed AFTER signing, from the actual cache and packaged runtime files.
// Evidence therefore tests the bytes published, rather than a local rebuild.
export function createCandidate({ sourceSha, artifact, cache, payload, now = new Date() }) {
  requireThat(/^[a-f0-9]{40}$/.test(sourceSha), 'A full source commit SHA is required');
  const flags = Object.fromEntries(readFileSync(cache, 'utf8').split(/\r?\n/)
    .flatMap(line => {
      const match = line.match(/^(COREVIDEO_[A-Z0-9_]+|BUILD_TESTING):BOOL=(ON|OFF)$/);
      return match ? [[match[1], match[2]]] : [];
    }).sort(([a], [b]) => a.localeCompare(b)));
  requireThat(flags.COREVIDEO_STUB === 'OFF', 'Release candidate must be a non-stub build');
  requireThat(flags.COREVIDEO_WITH_ZOOM === 'ON', 'Release candidate requires Zoom');
  const runtimeFiles = {};
  const walk = dir => {
    for (const item of readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const file = join(dir, item.name);
      if (item.isDirectory()) walk(file);
      else if (/\.(dll|exe|json)$/i.test(item.name))
        runtimeFiles[relative(payload, file).split(sep).join('/')] = sha256(readFileSync(file));
    }
  };
  walk(payload);
  requireThat(Object.keys(runtimeFiles).length > 0, 'Packaged runtime inventory is empty');
  const configuration = { platform: 'windows-x64', flags, runtimeFiles };
  return { schemaVersion: 1, sourceSha, createdAt: now.toISOString(), configuration,
    configurationSha256: sha256(JSON.stringify(configuration)),
    artifact: { name: basename(artifact), sha256: sha256(readFileSync(artifact)) } };
}

export function validateEvidence(candidate, evidence, { artifactBytes, readAttachment, now = new Date() } = {}) {
  requireThat(candidate.schemaVersion === 1 && evidence.schemaVersion === 1, 'Unsupported evidence schema');
  requireThat(/^[a-f0-9]{40}$/.test(candidate.sourceSha), 'Invalid candidate source SHA');
  requireThat(evidence.sourceSha === candidate.sourceSha, 'Evidence belongs to another source commit');
  requireThat(candidate.configurationSha256 === sha256(JSON.stringify(candidate.configuration)), 'Candidate configuration digest mismatch');
  requireThat(evidence.configurationSha256 === candidate.configurationSha256, 'Evidence belongs to another build configuration');
  requireThat(hashPattern.test(candidate.artifact.sha256) && evidence.artifactSha256 === candidate.artifact.sha256,
    'Evidence belongs to another artifact');
  requireThat(artifactBytes && sha256(artifactBytes) === candidate.artifact.sha256, 'Candidate artifact bytes do not match');
  const created = Date.parse(candidate.createdAt), started = Date.parse(evidence.startedAt), completed = Date.parse(evidence.completedAt);
  requireThat(Number.isFinite(created) && Number.isFinite(started) && Number.isFinite(completed)
    && started >= created && completed >= started && completed <= now.getTime()
    && now.getTime() - completed <= 7 * 24 * 60 * 60 * 1000, 'Missing, stale or invalid evidence timestamps');
  for (const key of ['rigId', 'os', 'gpu', 'driver', 'zoomSdkVersion', 'runtimeVersions', 'baselineId'])
    requireThat(nonempty(evidence.environment?.[key]), `Missing environment.${key}`);
  requireThat(Array.isArray(evidence.checks), 'Missing required checks');
  requireThat(new Set(evidence.checks.map(check => check.id)).size === evidence.checks.length, 'Duplicate checks');
  for (const id of requiredChecks) {
    const check = evidence.checks.find(item => item.id === id);
    requireThat(check?.status === 'passed', `Required check ${id} missing, skipped or failed`);
    requireThat(Array.isArray(check.attachments) && check.attachments.length > 0, `Missing attachments for ${id}`);
    for (const attachment of check.attachments) {
      requireThat(nonempty(attachment.path) && hashPattern.test(attachment.sha256), `Invalid attachment for ${id}`);
      requireThat(readAttachment && sha256(readAttachment(attachment.path)) === attachment.sha256, `Attachment mismatch for ${id}`);
    }
  }
  const soak = evidence.checks.find(check => check.id === 'simultaneous-soak');
  requireThat(Number.isFinite(soak.durationSeconds) && soak.durationSeconds >= 7200, 'Soak must last at least two hours');
  requireThat(completed - started >= soak.durationSeconds * 1000, 'Evidence interval is shorter than soak');
  for (const name of ['memorySlopeMbPerHour', 'frameDropPercent', 'queueDepth', 'commandP95Ms', 'commandMaxMs', 'avDriftMs']) {
    const metric = soak.metrics?.[name];
    requireThat(Number.isFinite(metric?.value) && metric.value >= 0 && Number.isFinite(metric?.maximum)
      && metric.maximum >= 0 && metric.value <= metric.maximum, `Missing or exceeded soak threshold: ${name}`);
  }
  requireThat(soak.metrics.commandP95Ms.maximum <= 250 && soak.metrics.commandMaxMs.maximum <= 1000,
    'Command latency thresholds exceed the release acceptance limits');
  return { status: 'passed', sourceSha: candidate.sourceSha, artifactSha256: candidate.artifact.sha256,
    configurationSha256: candidate.configurationSha256, checkedAt: now.toISOString(), checks: requiredChecks };
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const [command, ...args] = process.argv.slice(2);
    if (command === 'candidate' && args.length === 5) {
      const [sourceSha, artifact, cache, payload, output] = args;
      writeFileSync(output, JSON.stringify(createCandidate({ sourceSha, artifact, cache, payload }), null, 2) + '\n');
    } else if (command === 'validate' && args.length === 4) {
      const [manifest, evidenceFile, artifact, output] = args;
      const evidenceRoot = resolve(dirname(evidenceFile));
      const result = validateEvidence(readJson(manifest), readJson(evidenceFile), {
        artifactBytes: readFileSync(artifact),
        readAttachment: path => {
          const file = resolve(evidenceRoot, path);
          requireThat(file.startsWith(evidenceRoot + sep), 'Attachment must stay within evidence directory');
          return readFileSync(file);
        },
      });
      writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
    } else throw new Error('Usage: release-evidence.mjs candidate SHA MSIX CACHE PAYLOAD OUT | validate MANIFEST EVIDENCE MSIX OUT');
  } catch (error) { console.error(`Release evidence rejected: ${error.message}`); process.exitCode = 1; }
}
