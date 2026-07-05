// Click hunt: scan interleaved-stereo float32 raw PCM for sample-to-sample
// discontinuities and fingerprint their positions against packet (480-frame)
// and tick (960-frame) boundaries.
const fs = require('fs');

const path = process.argv[2];
const buf = fs.readFileSync(path);
const totalFloats = Math.floor(buf.length / 4);
const frames = Math.floor(totalFloats / 2);

let prevL = 0, prevR = 0;
let maxAbs = 0, sumSq = 0, nans = 0;
const clicks = [];
const THRESH = 0.12; // speech never jumps this much between adjacent 48k samples

for (let f = 0; f < frames; f++) {
  const l = buf.readFloatLE(f * 8);
  const r = buf.readFloatLE(f * 8 + 4);
  if (!isFinite(l) || !isFinite(r)) { nans++; continue; }
  const a = Math.max(Math.abs(l), Math.abs(r));
  if (a > maxAbs) maxAbs = a;
  sumSq += l * l + r * r;
  if (f > 0) {
    const dl = Math.abs(l - prevL), dr = Math.abs(r - prevR);
    if (dl > THRESH || dr > THRESH) {
      clicks.push({ frame: f, dl: +dl.toFixed(3), dr: +dr.toFixed(3) });
    }
  }
  prevL = l; prevR = r;
}

const rms = Math.sqrt(sumSq / (frames * 2));
console.log(`file=${path.split(/[\\/]/).pop()} frames=${frames} (${(frames/48000).toFixed(1)}s)`);
console.log(`maxAbs=${maxAbs.toFixed(3)} rms=${rms.toFixed(4)} (${(20*Math.log10(rms||1e-9)).toFixed(1)}dBFS) nans=${nans}`);
console.log(`clicks(threshold ${THRESH}): ${clicks.length}`);

if (clicks.length > 0) {
  // Boundary fingerprint: position of each click modulo packet/tick frames.
  const mod480 = {}, mod960 = {};
  for (const c of clicks) {
    const m4 = c.frame % 480, m9 = c.frame % 960;
    mod480[m4] = (mod480[m4] || 0) + 1;
    mod960[m9] = (mod960[m9] || 0) + 1;
  }
  const top = (m) => Object.entries(m).sort((a, b) => b[1] - a[1]).slice(0, 5)
    .map(([k, v]) => `${k}:${v}`).join(' ');
  console.log(`top mod480 positions: ${top(mod480)}`);
  console.log(`top mod960 positions: ${top(mod960)}`);
  console.log('first 12 clicks:', clicks.slice(0, 12).map(c => `${c.frame}(${c.dl}/${c.dr})`).join(' '));
  // Inter-click spacing tells periodicity.
  const gaps = [];
  for (let i = 1; i < Math.min(clicks.length, 40); i++) gaps.push(clicks[i].frame - clicks[i-1].frame);
  console.log('first click gaps:', gaps.slice(0, 20).join(' '));
}
