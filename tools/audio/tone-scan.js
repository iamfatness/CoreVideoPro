// Soak verdict scanner for stereo interleaved f32 PCM (the MON bus tap during
// a fake-engine tone soak). Emits PASS/FAIL on four independent checks:
//   1. clicks   - sample-to-sample discontinuities (threshold 0.12)
//   2. gaps     - silent stretches >50ms while the soak is running (tone
//                 should be continuous once flowing)
//   3. level    - per-second RMS stability (tones are steady; wobble >3dB
//                 between adjacent seconds = pumping/dropouts)
//   4. echo     - autocorrelation secondary peak >=0.3 in 20..400ms (a second
//                 delayed copy: double-routing or transport duplication)
// Usage: node tone-scan.js <file.f32> [--allow-clicks N]
const fs = require('fs');

const file = process.argv[2];
const allowClicks = (() => {
  const i = process.argv.indexOf('--allow-clicks');
  return i > 0 ? parseInt(process.argv[i + 1], 10) : 0;
})();
const buf = fs.readFileSync(file);
const frames = Math.floor(buf.length / 8);
const SR = 48000;
if (frames < SR * 5) {
  console.log('FAIL capture too short:', (frames / SR).toFixed(1) + 's');
  process.exit(1);
}

let prevL = 0;
let clicks = 0;
let silentRun = 0;
let maxSilentRun = 0;
let flowing = false;
const secRms = [];
let accum = 0;
const mono = new Float32Array(Math.floor(frames / 4));
for (let f = 0; f < frames; f++) {
  const l = buf.readFloatLE(f * 8);
  if (f % 4 === 0 && (f / 4) < mono.length) mono[f / 4] = l;
  accum += l * l;
  if ((f + 1) % SR === 0) {
    secRms.push(Math.sqrt(accum / SR));
    accum = 0;
  }
  const a = Math.abs(l);
  if (a < 1e-5) {
    silentRun++;
    if (flowing && silentRun > maxSilentRun) maxSilentRun = silentRun;
  } else {
    flowing = true;
    silentRun = 0;
  }
  if (f > 0 && flowing && Math.abs(l - prevL) > 0.12) clicks++;
  prevL = l;
}

// Level stability across the flowing region.
const active = secRms.filter(r => r > 1e-4);
let maxStepDb = 0;
for (let i = 1; i < active.length; i++) {
  const step = Math.abs(20 * Math.log10(active[i] / active[i - 1]));
  if (step > maxStepDb) maxStepDb = step;
}

// Echo autocorrelation on the decimated mono copy.
const N = mono.length;
let mean = 0;
for (let i = 0; i < N; i++) mean += mono[i];
mean /= N;
let r0 = 0;
for (let i = 0; i < N; i++) {
  mono[i] -= mean;
  r0 += mono[i] * mono[i];
}
let echoPeak = 0;
let echoLagMs = 0;
for (let lagMs = 20; lagMs <= 400; lagMs += 2) {
  const lag = Math.round((lagMs * SR) / 1000 / 4);
  let r = 0;
  for (let i = 0; i < N - lag; i++) r += mono[i] * mono[i + lag];
  const coeff = Math.abs(r / r0);
  if (coeff > echoPeak) {
    echoPeak = coeff;
    echoLagMs = lagMs;
  }
}

const gapMs = (maxSilentRun / SR) * 1000;
const checks = [
  ['clicks', clicks <= allowClicks, `${clicks} (allowed ${allowClicks})`],
  ['gaps', gapMs <= 50, `${gapMs.toFixed(0)}ms longest mid-flow silence`],
  ['level', maxStepDb <= 3, `${maxStepDb.toFixed(1)}dB max second-to-second step`],
  ['echo', echoPeak < 0.3, `autocorr ${echoPeak.toFixed(3)} @ ${echoLagMs}ms`],
];
let pass = true;
for (const [name, ok, detail] of checks) {
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}: ${detail}`);
  if (!ok) pass = false;
}
console.log(pass ? 'SOAK PASS' : 'SOAK FAIL');
process.exit(pass ? 0 : 1);
