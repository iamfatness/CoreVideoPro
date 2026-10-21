import { createReadStream, readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { assessProgramSourceAdmissions } from './program-source-admission-verdict.mjs';

const [samplesPath, measurementPath, ...expectedSourceIds] = process.argv.slice(2);
const measurement = JSON.parse(readFileSync(measurementPath, 'utf8').replace(/^\uFEFF/, ''));
const start = BigInt(measurement.start.qpc), end = BigInt(measurement.end.qpc);
const result = { schema: 'program-source-admission-capture-v1', samples: 0, failedSamples: 0,
  missingEvidenceSamples: 0, firstFailures: [], sourceAdmissionsVerified: false,
  physicalDisplayVerified: false, fullWorkloadPassed: false,
  limitation: 'Native source identity/admission observations, independent of output markers. Actual pixel, continuity and display evidence are separate requirements.' };
let rows = 0;
for await (const line of createInterface({ input: createReadStream(samplesPath), crlfDelay: Infinity })) {
  if (++rows > 100000) throw new Error('Sample count exceeds bounded capture');
  const row = JSON.parse(line.replace(/^\uFEFF/, ''));
  const at = BigInt(row.qpc);
  if (at < start || at > end) continue;
  ++result.samples;
  const verdict = assessProgramSourceAdmissions(row.snapshot, expectedSourceIds.length ? expectedSourceIds : undefined);
  if (verdict.issues.length) ++result.failedSamples;
  if (verdict.missingEvidence.length) ++result.missingEvidenceSamples;
  if (!verdict.sourceAdmissionsVerified && result.firstFailures.length < 8)
    result.firstFailures.push({ utc: row.utc, issues: verdict.issues, missingEvidence: verdict.missingEvidence });
}
result.sourceAdmissionsVerified = result.samples > 0 && !result.failedSamples && !result.missingEvidenceSamples;
process.stdout.write(JSON.stringify(result, null, 2) + '\n');
