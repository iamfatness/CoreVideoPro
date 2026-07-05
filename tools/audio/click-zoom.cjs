// Print L-channel samples around a position in two raw f32 stereo files.
const fs = require('fs');
const [,, fileA, fileB, centerStr] = process.argv;
const center = parseInt(centerStr, 10);
const read = (path) => {
  const buf = fs.readFileSync(path);
  const out = [];
  for (let f = center - 12; f <= center + 12; f++) {
    out.push(buf.readFloatLE(f * 8).toFixed(4));
  }
  return out;
};
console.log('frames', center - 12, '..', center + 12);
console.log('MON :', read(fileA).join(' '));
console.log('IN  :', read(fileB).join(' '));
