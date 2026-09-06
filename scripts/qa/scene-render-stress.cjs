// Mutates the current show. Run only in an authorized test meeting.
// Focused render ownership check: a command acknowledgment is not a rendered frame.
const fs = require('node:fs');
const path = require('node:path');
const base = process.env.COREVIDEO_TEST_API_URL || 'http://127.0.0.1:8011';
const output = path.resolve(process.env.COREVIDEO_TEST_OUTPUT_DIR || 'artifacts/render-stall/candidate');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const rows = [];
const startTime = new Date().toISOString();
fs.mkdirSync(output, { recursive: true });
async function state() {
  const response = await fetch(base + '/state', { signal: AbortSignal.timeout(10000) });
  if (!response.ok) throw new Error('State HTTP ' + response.status);
  return response.json();
}
async function invoke(action, args = []) {
  const response = await fetch(base + '/invoke', {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ action, args }), signal: AbortSignal.timeout(40000)
  });
  const result = await response.json();
  if (!response.ok || !result.ok) throw new Error(action + ': ' + (result.error || response.status));
}
function check(value, message) { if (!value) throw new Error(message); }
(async () => {
  const initial = await state();
  check(initial.engineOn && initial.zoomStatus === 'Zoom Live', 'Requires capture and a live Zoom meeting');
  check(typeof initial.nativeRenderPlanId === 'string', 'Requires rendered-plan diagnostics');
  if (initial.automationOn) await invoke('automation.toggle');
  for (let cycle = 0; cycle < 60; ++cycle) {
    const before = await state();
    check(before.engineOn && before.zoomStatus === 'Zoom Live', 'Capture or meeting stopped');
    const target = ['interview', 'panel', 'speaker-slides'][cycle % 3];
    await invoke('scene.select', [target]);
    check((await state()).activeSceneId === before.activeSceneId, 'Cue changed Program');
    const started = performance.now();
    if (target !== before.activeSceneId) await invoke('transport.take');
    let rendered;
    for (let attempt = 0; attempt < 50; ++attempt) {
      const current = await state();
      if (current.nativeActiveSceneId === target &&
          current.nativeRenderPlanId.startsWith(target + ':') &&
          current.nativeProgramFrameCount > before.nativeProgramFrameCount &&
          (target === before.activeSceneId || current.nativePreviewSceneId === before.activeSceneId)) {
        rendered = current;
        break;
      }
      await sleep(100);
    }
    check(rendered, 'No new rendered frame for ' + target);
    rows.push({ cycle: cycle + 1, target, renderPlanId: rendered.nativeRenderPlanId,
      frames: rendered.nativeProgramFrameCount, observedMs: Math.round(performance.now() - started) });
    if (cycle % 10 === 0) console.log(JSON.stringify(rows.at(-1)));
    await sleep(1000);
  }
  fs.writeFileSync(path.join(output, 'scene-render-results.json'), JSON.stringify({
    ok: true, startTime, endTime: new Date().toISOString(), cycles: rows.length, rows
  }, null, 2));
  console.log('PASS ' + rows.length + ' scene changes confirmed by rendered plan and advancing frames');
})().catch(error => {
  fs.writeFileSync(path.join(output, 'scene-render-results.json'), JSON.stringify({
    ok: false, startTime, endTime: new Date().toISOString(), error: error.message, rows
  }, null, 2));
  console.error(error);
  process.exitCode = 1;
});
