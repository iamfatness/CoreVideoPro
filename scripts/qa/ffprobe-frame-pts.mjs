// FFprobe's CSV frame view appends side-data labels after the timestamp.
// H.264 SEI padding makes that common, so parse the first field and require
// every nonblank row to carry a usable timestamp.
export function parseFfprobeFramePts(stdout) {
  if (!stdout.trim()) return [];
  return stdout.trim().split(/\r?\n/).map((line) => {
    const value = Number(line.split(",", 1)[0].trim());
    if (!Number.isFinite(value)) throw new Error(`invalid FFprobe frame timestamp: ${line}`);
    return value;
  });
}
