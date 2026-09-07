import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve, join } from 'node:path';
import { createHash } from 'node:crypto';
import { performance } from 'node:perf_hooks';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let stopRequested = false;

// Generated, uncompressed BMPs exercise real WIC pixels without FFmpeg or devices.
function fixture(path, index) {
  const width = 320, height = 180, pixels = width * height * 4;
  const bmp = Buffer.alloc(54 + pixels);
  bmp.write('BM'); bmp.writeUInt32LE(bmp.length, 2); bmp.writeUInt32LE(54, 10);
  bmp.writeUInt32LE(40, 14); bmp.writeInt32LE(width, 18); bmp.writeInt32LE(height, 22);
  bmp.writeUInt16LE(1, 26); bmp.writeUInt16LE(32, 28); bmp.writeUInt32LE(pixels, 34);
  for (let i = 54; i < bmp.length; i += 4) {
    bmp[i] = (index * 71 + 31) % 256; bmp[i + 1] = (index * 97 + 53) % 256;
    bmp[i + 2] = (index * 137 + 89) % 256; bmp[i + 3] = 255;
  }
  writeFileSync(path, bmp, { flag: 'wx' });
}

async function run(nativeCore, frames, fixtures, cycles) {
  const result = { frames, samples: [], errors: [], stderrTail: '', framePerformancePassed: false };
  const child = spawn(nativeCore, [], {
    cwd: dirname(nativeCore), windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'],
    env: { ...process.env, COREVIDEO_PROGRAM_BUFFER_FRAMES: String(frames) }
  });
  result.pid = child.pid;
  const pending = new Map();
  let stdout = '', nextId = 0, failure = null, exited = false;
  const fail = error => {
    failure ??= error;
    for (const entry of pending.values()) { clearTimeout(entry.timer); entry.reject(error); }
    pending.clear();
  };
  const exit = new Promise(resolveExit => child.once('close', (code, signal) => {
    exited = true;
    result.exitCode = code; result.exitSignal = signal;
    fail(new Error(`Native child closed (${code ?? signal}).`)); resolveExit();
  }));
  child.on('error', fail);
  child.stdin.on('error', fail);
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', text => { result.stderrTail = (result.stderrTail + text).slice(-65536); });
  child.stdout.on('data', chunk => {
    stdout += chunk;
    let newline;
    while ((newline = stdout.indexOf('\n')) >= 0) {
      const line = stdout.slice(0, newline).trim(); stdout = stdout.slice(newline + 1);
      if (!line) continue;
      if (line.length > 1024 * 1024) { fail(new Error('RPC line exceeds 1 MiB bound.')); child.kill(); return; }
      let message;
      try { message = JSON.parse(line); } catch { fail(new Error('Invalid JSON on native stdout.')); continue; }
      if (message.type === 'handshake') result.profile = message.profile;
      const entry = pending.get(String(message.id));
      if (!entry) continue;
      clearTimeout(entry.timer); pending.delete(String(message.id));
      if (message.ok !== true) entry.reject(new Error(`RPC rejected: ${JSON.stringify(message.error ?? message).slice(0, 1000)}`));
      else entry.resolve(message);
    }
    if (stdout.length > 1024 * 1024) { fail(new Error('Unterminated RPC line exceeds 1 MiB bound.')); child.kill(); }
  });
  const request = (type, fields = {}) => new Promise((resolveRequest, reject) => {
    if (failure) { reject(failure); return; }
    const id = `buffer-smoke-${++nextId}`;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out after 5 seconds.`)); }, 5000);
    pending.set(id, { resolve: resolveRequest, reject, timer });
    child.stdin.write(JSON.stringify({ id, type, ...fields }) + '\n');
  });
  const interrupted = () => { stopRequested = true; fail(new Error('Smoke interrupted.')); child.kill(); };
  process.once('SIGINT', interrupted); process.once('SIGTERM', interrupted);
  try {
    await request('handshake', { protocolVersion: { major: 1, minor: 0 } });
    const sync = commands => request('media-core-sync', { commands });
    await sync([
      { type: 'set-output-profile', width: 1920, height: 1080, fps: 60 },
      { type: 'configure-multiviewer', layoutMode: 'pgmPvwTop', tileCount: 8 },
      { type: 'start-program-output', destinations: [], destinationSettings: [] }
    ]);
    let previousFrame = 0, previousPreview = 0;
    for (let cycle = 0; cycle < cycles; ++cycle) {
      if (stopRequested) throw new Error('Interrupted.');
      const count = [1, 2, 4, 8][cycle % 4];
      // Each activation has new asset identities so still-frame freshness does not
      // expire while this test waits. This does not simulate continuous camera video.
      const ids = fixtures.slice(0, count).map((_, i) => `tiles-${cycle}-${i}`);
      const routes = ids.map((id, i) => ({ routeId: `fixture-${i}`, mode: 'fixed', zIndex: -20 + i,
        mediaAssetId: id, mediaAssetKind: 'image', mediaAssetPath: fixtures[i],
        rect: { x: 0, y: 0, width: 1, height: 1 } }));
      const sceneId = `tiles-program-${cycle}`, previewId = `tiles-preview-${cycle}`;
      const members = ids.map(id => `media:${id}`);
      const tile = values => ({ layerId: `wall-${cycle}`, order: 0,
        rect: { x: 0, y: 0, w: 1, h: 1 }, members: [...values, 'zoom:never-present'],
        style: { tileAspect: cycle % 2 ? '1:1' : '16:9', gutterPercent: 0.741, marginPercent: 0.741 } });
      await sync([
        { type: 'load-scene-graph', sceneId, routes, tiles: tile(members) },
        { type: 'set-preview-scene', sceneId: previewId, routes,
          tiles: tile(ids.map(id => `preview:media:${id}`).reverse()) }
      ]);
      const deadline = performance.now() + 5000;
      let snapshot, matched = false;
      do {
        snapshot = (await request('snapshot')).snapshot;
        const frame = snapshot?.programFrame;
        const actual = frame?.videoSources?.filter(source => source.layerId.startsWith('tile:')).map(source => source.sourceId);
        matched = frame?.sceneId === sceneId && frame?.frameNumber > previousFrame &&
          frame?.renderPlanId?.startsWith(sceneId + ':') && JSON.stringify(actual) === JSON.stringify(members) &&
          snapshot?.previewScene?.sceneId === previewId && snapshot?.previewScene?.composite === true &&
          snapshot?.previewSharedTexture?.frameNumber > previousPreview;
        if (matched) break;
        await sleep(40);
      } while (performance.now() < deadline);
      if (!matched) {
        result.failedSnapshot = snapshot;
        throw new Error(`Cycle ${cycle}: actual Tiles membership and Program/Preview progress did not converge within 5 seconds.`);
      }
      const frame = snapshot.programFrame;
      if (!frame.gpuComposed || !/d3d11/i.test(frame.renderer)) throw new Error('Actual D3D11 composition was not confirmed.');
      if (frame.width !== 1920 || frame.height !== 1080 || frame.fps !== 60) throw new Error('1080p60 configuration changed.');
      if (snapshot.programBuffer?.activeFrames !== frames) throw new Error('Requested Program buffer depth is not active.');
      result.samples.push({ cycle, count, programFrame: frame, previewScene: snapshot.previewScene,
        previewFrameNumber: snapshot.previewSharedTexture.frameNumber, programBuffer: snapshot.programBuffer });
      previousFrame = frame.frameNumber;
      previousPreview = snapshot.previewSharedTexture.frameNumber;
      // Removing the wall must converge too; never accept a stale Tiles snapshot
      // just because desired scene state acknowledged the command.
      const plainId = `plain-${cycle}`;
      await sync([{ type: 'load-scene-graph', sceneId: plainId, routes: [] }]);
      const clearDeadline = performance.now() + 5000;
      do {
        snapshot = (await request('snapshot')).snapshot;
        if (snapshot?.programFrame?.sceneId === plainId && snapshot.programFrame.frameNumber > previousFrame && !snapshot.tiles) break;
        await sleep(40);
      } while (performance.now() < clearDeadline);
      if (snapshot?.programFrame?.sceneId !== plainId || !(snapshot.programFrame.frameNumber > previousFrame) || snapshot.tiles)
        throw new Error(`Cycle ${cycle}: switching away from Tiles stalled.`);
      previousFrame = snapshot.programFrame.frameNumber;
    }
  } catch (error) {
    result.errors.push(error.message);
  } finally {
    child.stdin.end();
    if (!exited) await Promise.race([exit, sleep(3000)]);
    if (!exited) { result.forcedChildCleanup = true; result.errors.push('Owned child required forced shutdown.'); child.kill(); await Promise.race([exit, sleep(1000)]); }
    if (!exited) result.errors.push('Owned child did not confirm shutdown.');
    else if (result.exitCode !== 0) result.errors.push(`Owned child exited abnormally (${result.exitCode ?? result.exitSignal}).`);
    process.removeListener('SIGINT', interrupted); process.removeListener('SIGTERM', interrupted);
  }
  result.nativeTilesPassed = result.errors.length === 0;
  return result;
}

try {
  const options = {}, args = process.argv.slice(2);
  for (let i = 0; i < args.length; i += 2) {
    if (!['--native-core', '--output', '--cycles'].includes(args[i]) || !args[i + 1])
      throw new Error('Usage: node scripts/qa/tiles-scene-smoke.mjs --native-core EXE --output REPORT.json [--cycles 12]');
    options[args[i]] = args[i + 1];
  }
  if (!options['--native-core'] || !options['--output']) throw new Error('Explicit executable and output required.');
  const nativeCore = resolve(options['--native-core']), output = resolve(options['--output']);
  const cycles = Number(options['--cycles'] ?? 12);
  if (!Number.isInteger(cycles) || cycles < 4 || cycles > 32) throw new Error('Cycles must be 4..32 per depth.');
  if (!existsSync(nativeCore) || existsSync(output)) throw new Error('Executable missing or report already exists.');
  mkdirSync(dirname(output), { recursive: true });
  const fixtureDirectory = output + '.fixtures';
  mkdirSync(fixtureDirectory); // exclusive directory; retain fixtures with evidence
  const fixtures = Array.from({ length: 8 }, (_, i) => join(fixtureDirectory, `${i}.bmp`));
  fixtures.forEach(fixture);
  const report = { schemaVersion: 1, nativeCore, nativeSha256: createHash('sha256').update(readFileSync(nativeCore)).digest('hex'),
    scope: 'headless-native-tiles-functional', cycles, physicalPresentationMeasured: false, framePerformancePassed: false,
    limitation: 'Real D3D/WIC still-image Tiles on Program and Preview; no GUI consumer, live meeting, camera video, physical presentation, or output cadence acceptance. Media backing routes sit below the Tiles background. Preview validates composite surface progress, not pixel membership.', runs: [] };
  for (const frames of [2, 3]) {
    if (stopRequested) break;
    report.runs.push(await run(nativeCore, frames, fixtures, cycles));
  }
  report.nativeTilesPassed = report.runs.length === 2 && report.runs.every(run => run.nativeTilesPassed);
  writeFileSync(output, JSON.stringify(report, null, 2), { flag: 'wx' });
  console.log(JSON.stringify({ report: output, passed: report.nativeTilesPassed,
    runs: report.runs.map(run => ({ frames: run.frames, cycles: run.samples.length, exitCode: run.exitCode, errors: run.errors })) }));
  if (!report.nativeTilesPassed) process.exitCode = 1;
} catch (error) { console.error(error.message); process.exitCode = 1; }
