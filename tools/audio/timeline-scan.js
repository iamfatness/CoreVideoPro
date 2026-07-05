// Timeline + low-level adaptive click scan for interleaved-stereo f32 PCM.
// Buckets per 10s: RMS (dBFS), hard clicks (delta>0.12), soft clicks
// (delta > max(0.02, 8*localRms) with localRms<0.02 — artifacts in the floor).
const fs = require('fs');
const buf = fs.readFileSync(process.argv[2]);
const frames = Math.floor(buf.length / 8);
const SR = 48000, BUCKET = 10 * SR;
const buckets = [];
let prevL = 0;
let localAccum = 0;
const WIN = 2048;
const window = new Float32Array(WIN);
let winIdx = 0, winSum = 0;

for (let f = 0; f < frames; f++) {
  const b = Math.floor(f / BUCKET);
  if (!buckets[b]) buckets[b] = { sumSq: 0, n: 0, hard: 0, soft: 0 };
  const l = buf.readFloatLE(f * 8);
  buckets[b].sumSq += l * l;
  buckets[b].n++;
  const a = Math.abs(l);
  winSum += a - window[winIdx];
  window[winIdx] = a;
  winIdx = (winIdx + 1) % WIN;
  const localAvg = winSum / WIN;
  if (f > 0) {
    const d = Math.abs(l - prevL);
    if (d > 0.12) buckets[b].hard++;
    else if (localAvg < 0.02 && d > Math.max(0.02, 8 * localAvg)) buckets[b].soft++;
  }
  prevL = l;
}

console.log('sec | dBFS  | hard | soft(floor)');
buckets.forEach((bk, i) => {
  const rms = Math.sqrt(bk.sumSq / bk.n);
  const db = (20 * Math.log10(rms || 1e-9)).toFixed(1);
  console.log(`${String(i * 10).padStart(3)} | ${String(db).padStart(6)} | ${String(bk.hard).padStart(4)} | ${bk.soft}`);
});
