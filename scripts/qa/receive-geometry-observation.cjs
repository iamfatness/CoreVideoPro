// Read-only observation. Does not join, change subscriptions, or start outputs.
const fs = require('node:fs');
const output = process.env.COREVIDEO_TEST_OUTPUT_FILE;
if (!output || fs.existsSync(output)) throw Error('Choose a new COREVIDEO_TEST_OUTPUT_FILE');
const seconds = Number(process.env.COREVIDEO_TEST_SECONDS || 30);
if (!Number.isFinite(seconds) || seconds < 1 || seconds > 1800) throw Error('Duration must be 1–1800 seconds');
const result = { startedUtc: new Date().toISOString(), samples: [] };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
  const started = performance.now();
  while (performance.now() - started < seconds * 1000) {
    const response = await fetch('http://127.0.0.1:8011/snapshot', { signal: AbortSignal.timeout(10000) });
    if (!response.ok) throw Error('Snapshot unavailable');
    const { snapshot: s } = await response.json();
    const stateResponse = await fetch('http://127.0.0.1:8011/state', { signal: AbortSignal.timeout(10000) });
    if (!stateResponse.ok) throw Error('Control state unavailable');
    const state = await stateResponse.json();
    result.samples.push({ elapsedMs: Math.round(performance.now() - started), meetingState: s.meetingState,
      rosterEpoch: s.rosterEpoch, recording: state.recording, streaming: state.streaming,
      recordingLifecycle: s.recording?.lifecycle,
      writers: (s.recording?.streams || []).map(x => ({ kind: x.kind, sourceId: x.sourceId,
        status: x.status, framesWritten: x.framesWritten, bytesWritten: x.bytesWritten })),
      sources: (s.sources || []).filter(x => x.kind === 'zoom' && /^\d+$/.test(x.sourceId)).map(x => ({
        id: x.sourceId, width: x.width, height: x.height, frames: x.framesIngested,
        ageMs: x.sinceLastNewFrameMs, dropped: x.droppedFrames })),
      subscriptions: s.zoomSubscriptionChurn,
    });
    await sleep(1000);
  }
  result.completed = true;
})().catch(() => { result.completed = false; result.error = 'Observation interrupted or snapshot unavailable'; process.exitCode = 1; })
  .finally(() => { result.endedUtc = new Date().toISOString(); fs.writeFileSync(output, JSON.stringify(result, null, 2));
    console.log(JSON.stringify({ completed: result.completed, samples: result.samples.length })); });
