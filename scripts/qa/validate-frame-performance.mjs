import { readFileSync, statSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

// Read-only evidence gate. Average FPS and operator-cycle success are never
// substitutes for exact, whole-interval deadline/completion evidence.
export const METRIC_VERSION = 'anchored-deadline-v1';
const MAX_REPORT_BYTES = 4 * 1024 * 1024;
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const count = value => Number.isSafeInteger(value) && value >= 0;
const stamp = value => typeof value === 'string' && /(?:Z|[+-]\d\d:\d\d)$/.test(value) ? Date.parse(value) : NaN;

export function validateFramePerformance(report) {
  const issues = [], missingEvidence = [], diagnostics = [];
  const fail = message => issues.push(message);
  const missing = message => missingEvidence.push(message);
  if (!object(report)) report = {};
  const start = stamp(report.startTime), end = stamp(report.endTime);
  const validInterval = Number.isFinite(start) && Number.isFinite(end) && end > start;
  if (!validInterval) missing('Missing or invalid finalized measurement interval.');
  const operatorFunctionalPassed = report.soakPassed === true;
  if (!Array.isArray(report.errorMatches)) missing('Missing bounded error coverage.');
  else if (report.errorMatches.length) fail(`Report contains ${report.errorMatches.length} error(s).`);
  if (typeof report.reportedDrops === 'number' && report.reportedDrops > 0)
    diagnostics.push(`Legacy telemetry reports ${report.reportedDrops} late loop intervals (not an exact missed-frame count).`);

  const evidence = report.performanceEvidence;
  if (!object(evidence) || evidence.metricVersion !== METRIC_VERSION) {
    missing('Missing versioned anchored-deadline evidence; rounded loop-start maxima and average FPS cannot prove every frame.');
  } else {
    if (evidence.targetFps !== 60 || evidence.clock !== 'monotonic') fail('Evidence must use monotonic anchored 60 FPS deadlines.');
    const required = evidence.requiredPaths;
    const paths = evidence.paths;
    if (!Array.isArray(required) || !required.length || required.some(id => typeof id !== 'string' || !id.trim()) ||
        new Set(required).size !== required.length || !['cpu-submission', 'program-gpu', 'program-presentation'].every(id => required.includes(id)))
      missing('Required paths must uniquely declare cpu-submission, program-gpu, program-presentation and every enabled output path.');
    if (!Array.isArray(evidence.enabledOutputs) || evidence.enabledOutputs.some(id => typeof id !== 'string' || !id.trim()) ||
        new Set(evidence.enabledOutputs).size !== evidence.enabledOutputs.length)
      missing('Enabled presentation/export paths must be explicitly inventoried (an empty list means none enabled).');
    else for (const id of evidence.enabledOutputs) if (!Array.isArray(required) || !required.includes(id)) missing(`Enabled output ${id} has no required evidence path.`);
    if (!Array.isArray(paths) || !paths.length || paths.length > 64) missing('Missing or unbounded per-path measurements.');
    else {
      const seen = new Set();
      for (const path of paths) {
        if (!object(path) || typeof path.id !== 'string' || !path.id.trim()) { fail('Invalid measurement path.'); continue; }
        if (seen.has(path.id)) fail(`Duplicate measurement path ${path.id}.`);
        seen.add(path.id);
        const prefix = path.id + ': ';
        if (!Array.isArray(required) || !required.includes(path.id)) fail(prefix + 'undeclared path.');
        const coverage = path.coverage;
        if (!object(coverage) || coverage.complete !== true || !validInterval ||
            !Number.isFinite(stamp(coverage.startTime)) || !Number.isFinite(stamp(coverage.endTime)) ||
            stamp(coverage.startTime) > start || stamp(coverage.endTime) < end)
          missing(prefix + 'incomplete interval coverage, including boundary/partial windows.');
        for (const field of ['firstSlot', 'lastSlot', 'expectedSlots', 'completedSlots', 'uniqueCompletedSlots', 'deadlineMisses', 'skippedSlots'])
          if (!count(path[field])) missing(prefix + `missing/invalid ${field}.`);
        if (path.id === 'cpu-submission') {
          // A bounded buffer can absorb producer timing variation. Acceptance is
          // measured at scheduled GPU readiness and downstream delivery, never
          // against the producer's earlier unbuffered completion deadline.
          diagnostics.push(prefix + `completed=${path.completedSlots}, deadlineMisses=${path.deadlineMisses}, skippedSlots=${path.skippedSlots}.`);
          if (!Array.isArray(path.errors)) missing(prefix + 'missing error evidence.');
          else if (path.errors.length) fail(prefix + 'measurement errors.');
          if (path.measurement !== 'completion-deadlines') missing(prefix + 'requires actual completion deadlines, not loop-start timing.');
          continue;
        }
        if (!count(path.expectedSlots) || path.expectedSlots === 0) fail(prefix + 'zero measured slots.');
        if (validInterval && count(path.expectedSlots) && path.expectedSlots < Math.floor((end - start) * 60 / 1000))
          fail(prefix + 'slot count cannot cover the requested duration at 60 FPS.');
        if (count(path.expectedSlots) && path.lastSlot - path.firstSlot + 1 !== path.expectedSlots)
          fail(prefix + 'slot sequence coverage is incomplete.');
        if (path.completedSlots !== path.expectedSlots || path.uniqueCompletedSlots !== path.expectedSlots)
          fail(prefix + 'missing or duplicate frame completions.');
        if (count(path.deadlineMisses) && path.deadlineMisses !== 0) fail(prefix + 'deadline misses must be zero.');
        if (count(path.skippedSlots) && path.skippedSlots !== 0) fail(prefix + 'skipped slots must be zero.');
        if (!Array.isArray(path.errors)) missing(prefix + 'missing error evidence.');
        else if (path.errors.length) fail(prefix + 'measurement errors.');
        if (path.measurement !== 'completion-deadlines') missing(prefix + 'requires actual completion deadlines, not loop-start timing.');
        // Presentation intervals, when supplied, must be exact raw durations;
        // never compare the old rounded worstFrameMaxMs to a fractional limit.
        if (path.worstPresentationIntervalNs !== undefined &&
            (!count(path.worstPresentationIntervalNs) || path.worstPresentationIntervalNs > Math.ceil(1e9 / 60)))
          fail(prefix + 'presentation interval exceeds the nanosecond-quantized 1/60 second limit or is invalid.');
      }
      if (Array.isArray(required)) for (const id of required) if (!seen.has(id)) missing(`Missing required path ${id}.`);
    }
  }
  return { status: issues.length ? 'failed' : missingEvidence.length ? 'insufficient-evidence' : 'passed', operatorFunctionalPassed,
    framePerformancePassed: issues.length === 0 && missingEvidence.length === 0,
    evidenceComplete: missingEvidence.length === 0, targetFps: 60, issues: [...issues, ...missingEvidence], diagnostics };
}

export function readBoundedReport(file) {
  if (statSync(file).size > MAX_REPORT_BYTES) throw new Error('Report exceeds 4 MiB bounded evidence limit.');
  return JSON.parse(readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    if (process.argv.length < 3 || process.argv.length > 4) throw new Error('Usage: node scripts/qa/validate-frame-performance.mjs REPORT.json [VERDICT.json]');
    if (process.argv[3] && resolve(process.argv[2]) === resolve(process.argv[3])) throw new Error('Verdict must not overwrite source evidence.');
    const verdict = validateFramePerformance(readBoundedReport(process.argv[2]));
    const output = JSON.stringify(verdict, null, 2) + '\n';
    if (process.argv[3]) writeFileSync(process.argv[3], output);
    process.stdout.write(output);
    process.exitCode = verdict.framePerformancePassed ? 0 : 1;
  } catch (error) {
    process.stderr.write(`Frame performance validation failed: ${error.message}\n`);
    process.exitCode = 1;
  }
}
