// Mutates the current show: use only in an authorized test meeting. No meeting credentials are collected.
// Native snapshots and immutable shell diagnostics are consecutive observations, correlated by epoch/time.
const fs = require('node:fs');
const base = 'http://127.0.0.1:8011';
const output = require('node:path').resolve(process.env.COREVIDEO_TEST_OUTPUT_FILE || 'auto-decision-rehearsal.json');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const result = { source: process.env.COREVIDEO_TEST_SOURCE || 'unspecified', workload: 'real populated test meeting', startedUtc: new Date().toISOString(), checks: [], samples: [] };
async function read(path) { const r = await fetch(base + path, { signal: AbortSignal.timeout(10000) }); if (!r.ok) throw Error(path + ' read failed'); return r.json(); }
async function invoke(action, args = [], expectRefusal = false) {
  const response = await fetch(base + '/invoke', { method: 'POST', headers: {'content-type': 'application/json'}, body: JSON.stringify({action, args}), signal: AbortSignal.timeout(30000) });
  const reply = await response.json();
  if (expectRefusal ? reply.ok : !reply.ok) throw Error(action + (expectRefusal ? ' unexpectedly accepted' : ' refused'));
  return reply;
}
function check(condition, label) { result.checks.push({label, passed: !!condition}); if (!condition) throw Error(label); }
async function take(scene) {
  await invoke('scene.select', [scene]);
  const selected = await read('/state');
  if (selected.activeSceneId === scene && selected.nativeActiveSceneId === scene) {
    check(true, 'baseline scene already confirmed on native Program');
    return;
  }
  const deadline = Date.now() + 12000;
  while (!(await read('/state')).canTake && Date.now() < deadline) await sleep(200);
  check((await read('/state')).canTake, 'manual scene becomes Take-ready');
  await invoke('transport.take');
  const confirm = Date.now() + 5000;
  while ((await read('/state')).nativeActiveSceneId !== scene && Date.now() < confirm) await sleep(100);
  check((await read('/state')).nativeActiveSceneId === scene, 'manual Take confirmed in native Program');
}
(async () => {
  const initial = await read('/state');
  const snapshot = (await read('/snapshot')).snapshot;
  check(snapshot.meetingState === 'in_meeting', 'real meeting joined');
  const remoteIds = new Set(snapshot.participants.filter(p => p.videoOn && !p.isMe).map(p => String(p.userId)));
  check(remoteIds.size >= 2, 'at least two remote cameras available');
  check(initial.automationDiagnostic, 'shell Auto diagnostic available');
  result.receivedGeometry = snapshot.sources.filter(s => s.kind === 'zoom' && remoteIds.has(s.sourceId)).map(s => ({id:s.sourceId,width:s.width,height:s.height,frames:s.framesIngested}));
  check(!initial.recording && !initial.streaming, 'no recording or streaming during directing rehearsal');
  await invoke('automation.captions.set', [true], true);
  check(!(await read('/state')).autoCaptions, 'nonexistent Auto captions cannot be enabled');
  if (initial.automationOn) await invoke('automation.toggle');
  await invoke('automation.magic');
  const target = (await read('/state')).previewSceneId;
  const baseline = target === 'interview' ? 'panel' : 'interview';
  await take(baseline);
  await invoke('automation.magic');
  await invoke('automation.autoTake.set', [true]);
  await invoke('automation.toggle');
  const started = Date.now();
  let autoChangedProgram = false;
  while (Date.now() - started < 60000) {
    const state = await read('/state');
    const native = (await read('/snapshot')).snapshot;
    const ids = (state.nativeProgramVideoSources || []).filter(s => s.kind === 'participant-video').map(s => String(s.participantId));
    const bindings = native.autoProduction?.slotBindings || [];
    const boundIds = bindings.map(b => String(b.personId));
    const sample = {elapsedMs:Date.now()-started, automation:state.automationOn, captions:state.autoCaptions, shellScene:state.activeSceneId, nativeScene:state.nativeActiveSceneId, programIds:ids, rule:native.autoProduction?.ruleId, recommendation:native.autoProduction?.recommendedSceneId, bindings, diagnostic:state.automationDiagnostic, canTake:state.canTake, previewSceneId:state.previewSceneId, commandStatus:state.commandStatus};
    result.samples.push(sample);
    if (state.autoCaptions) throw Error('Auto enabled nonexistent captions');
    if (new Set(ids).size !== ids.length || ids.some(id => !remoteIds.has(id))) throw Error('Program has duplicate or ineligible remote binding');
    if (new Set(boundIds).size !== boundIds.length || boundIds.some(id => !remoteIds.has(id))) throw Error('Recommendation has duplicate or ineligible remote binding');
    if (state.activeSceneId !== baseline && state.nativeActiveSceneId === state.activeSceneId) autoChangedProgram = true;
    await sleep(500);
  }
  check(autoChangedProgram, 'Auto changed Program and native confirmed the scene');
  check(result.samples.every(s => s.automation && !s.captions), 'Auto stayed active without captions');
  check(result.samples.every(s => new Set(s.programIds).size === s.programIds.length), 'all observed Program participant identities unique');
  check((await read('/state')).nativeLowerThirdVisible, 'legacy lower third was actually visible before Hide');
  await invoke('graphics.lowerThird.set', [false]);
  await sleep(3000);
  const lower = await read('/state');
  check(lower.automationOn && !lower.nativeLowerThirdVisible, 'manual lower-third off survives Auto');
  const manual = lower.nativeActiveSceneId === 'interview' ? 'panel' : 'interview';
  await take(manual);
  check(!(await read('/state')).automationOn, 'manual cue pauses Auto');
  await sleep(10000);
  const final = await read('/state');
  check(!final.automationOn && final.nativeActiveSceneId === manual && final.activeSceneId === manual, 'manual Take remains authoritative');
  check(final.nativeProgramFrameCount > initial.nativeProgramFrameCount, 'Program continued producing frames');
  check(!final.recording && !final.streaming, 'no public or recording output started');
  result.passed = true;
})().catch(error => { result.passed = false; result.error = error.message; process.exitCode = 1; }).finally(async () => {
  try { if ((await read('/state')).automationOn) await invoke('automation.toggle'); } catch {}
  result.endedUtc = new Date().toISOString();
  fs.writeFileSync(output, JSON.stringify(result, null, 2));
  console.log(JSON.stringify({passed:result.passed, checks:result.checks.length, samples:result.samples.length, error:result.error || null}));
});


