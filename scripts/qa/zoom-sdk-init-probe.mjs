// Credential-free SDK initialization probe. Never sends auth credentials or joins.
import fs from 'node:fs/promises';
import { constants } from 'node:fs';
import path from 'node:path';
import { randomUUID, createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { parseArgs } from 'node:util';
const { values } = parseArgs({ options: { exe: { type: 'string' }, output: { type: 'string' } } });
if (!values.exe || !values.output) throw new Error('Required --exe and --output (report file)');
if (process.platform !== 'win32') throw new Error('This probe requires Windows named pipes');
const exe = await fs.realpath(values.exe);
const output = path.resolve(values.output);
await fs.mkdir(path.dirname(output), { recursive: true });
const report = { success: false, joinedMeeting: false, credentialsSent: false,
  startTime: new Date().toISOString(), executableSha256: createHash('sha256').update(await fs.readFile(exe)).digest('hex'), stages: [] };
const token = `sdk-init-${randomUUID()}`;
const pipe = direction => ['', '', '.', 'pipe', `ZoomObsPlugin_${token}_${direction}`].join(String.fromCharCode(92));
let child, writer, reader, stopping = false, exited = false, timedOut = false, initResolve;
const initObserved = new Promise(resolve => { initResolve = resolve; });
let cancelled = false;
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => { cancelled = true; stopping = true; initResolve(); });
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const deadline = Date.now() + 8000;
async function connect(name, mode) {
  while (!stopping && !exited && Date.now() < deadline) {
    try { return await fs.open(name, mode); }
    catch (error) {
      if (!['ENOENT', 'EBUSY', 'EACCES'].includes(error.code)) throw new Error(`Pipe open failed (${error.code ?? 'unknown'})`);
      await sleep(50);
    }
  }
  throw new Error('Pipe connection timed out or child exited');
}
async function receive() {
  const bytes = Buffer.alloc(8192);
  let pending = '';
  while (!stopping) {
    const { bytesRead } = await reader.read(bytes, 0, bytes.length, null);
    if (!bytesRead) break;
    pending += bytes.toString('utf8', 0, bytesRead);
    if (pending.length > 65536) throw new Error('Oversized engine response');
    for (;;) {
      const end = pending.indexOf('\n'); if (end < 0) break;
      const line = pending.slice(0, end); pending = pending.slice(end + 1);
      let event; try { event = JSON.parse(line); } catch { continue; }
      // Fixed allowlist: never persist arbitrary engine messages or process logs.
      if (event.cmd === 'debug' && ['init_received', 'before_init_sdk', 'after_init_sdk',
        'before_create_auth', 'after_create_auth'].includes(event.stage)) {
        report.stages.push({ stage: event.stage, code: Number.isInteger(event.code) ? event.code : null });
        if (event.stage === 'after_init_sdk') {
          report.initCode = event.code;
          report.success = event.code === 0;
          initResolve();
        }
      }
    }
  }
}
let timeout, readTask, exitPromise;
try {
  child = spawn(exe, ['--ipc-token', token], { cwd: path.dirname(exe), windowsHide: true, stdio: 'ignore' });
  report.ownedPid = child.pid;
  exitPromise = new Promise(resolve => {
    child.once('exit', (code, signal) => { exited = true; report.exitCode = code; report.exitSignal = signal; resolve(); initResolve(); });
    child.once('error', () => { exited = true; report.error = 'Child launch failed'; resolve(); initResolve(); });
  });
  const timeoutPromise = new Promise(resolve => {
    timeout = setTimeout(() => { timedOut = true; report.error = 'SDK init probe timed out'; initResolve(); resolve(); }, 8000);
  });
  const work = (async () => {
    // Helper creates the directional pipes; connect P2E before E2P as its setup expects.
    writer = await connect(pipe('P2E'), constants.O_WRONLY);
    reader = await connect(pipe('E2P'), constants.O_RDONLY);
    readTask = receive().catch(() => { if (!stopping) report.readError = 'Engine response pipe closed or failed'; initResolve(); });
    await writer.writeFile('{"cmd":"init"}\n');
    await initObserved;
  })();
  await Promise.race([work, timeoutPromise]);
  work.catch(() => {});
  if (timedOut) report.success = false;
} catch (error) { report.error = error.message; report.success = false; }
finally {
  clearTimeout(timeout);
  if (cancelled) { report.success = false; report.error = 'Cancelled'; }
  stopping = true;
  if (writer && !exited) {
    // Quit is the only command besides init; do not wait indefinitely on pipe IO.
    await Promise.race([writer.writeFile('{"cmd":"quit"}\n').catch(() => {}), sleep(250)]);
  }
  if (exitPromise && !exited) await Promise.race([exitPromise, sleep(1000)]);
  if (child && !exited) {
    report.forcedOwnedChildStop = true;
    child.kill();
    await Promise.race([exitPromise, sleep(3000)]);
  }
  report.ownedChildExited = exited;
  if (!exited) { report.success = false; report.cleanupError = 'Owned child did not exit'; }
  // Child exit breaks outstanding native pipe reads before handle close.
  if (exited) {
    await Promise.race([readTask, sleep(1000)]);
    await Promise.race([Promise.allSettled([reader?.close(), writer?.close()]), sleep(1000)]);
  }
  report.endTime = new Date().toISOString();
  await fs.writeFile(output, JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
  if (!report.success) process.exitCode = 1;
}
