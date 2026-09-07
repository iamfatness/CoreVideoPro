import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

test('restoration fails when local Program matches but native Program diverges', { timeout: 20000 }, async () => {
  let preview = 'original-preview';
  let frame = 1;
  const server = http.createServer(async (req, res) => {
    res.setHeader('content-type', 'application/json');
    if (req.url === '/state') {
      res.end(JSON.stringify({ engineOn: true, zoomStatus: 'Zoom Live', automationOn: false,
        autoTake: false, recording: false, streaming: false, activeSceneId: 'original-program',
        previewSceneId: preview, nativeActiveSceneId: 'different-native-program',
        nativeRenderedSceneId: 'different-native-program', nativeRenderPlanId: 'different-native-program:plan',
        nativePreviewSceneId: preview, nativeProgramFrameCount: frame++, viewMode: 'ProgramPreview' }));
      return;
    }
    let body = ''; for await (const chunk of req) body += chunk;
    const command = JSON.parse(body);
    if (command.action === 'scene.select') preview = command.args[0];
    res.end(JSON.stringify({ ok: command.action !== 'transport.take' }));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const output = await fs.mkdtemp(path.join(os.tmpdir(), 'tiles-selection-test-'));
  try {
    const child = spawn(process.execPath, ['scripts/qa/tiles-live-selection.mjs',
      '--base', `http://127.0.0.1:${server.address().port}`, '--tiles-scene', 'tiles',
      '--other-scene', 'other', '--output', output, '--cycles', '1'],
      { windowsHide: true, stdio: 'ignore', env: { ...process.env, COREVIDEO_TEST_API_TOKEN: '' } });
    const code = await new Promise((resolve, reject) => { child.on('exit', resolve); child.on('error', reject); });
    const report = JSON.parse(await fs.readFile(path.join(output, 'tiles-live-selection-results.json'), 'utf8'));
    assert.equal(code, 1);
    assert.equal(report.ok, false);
    assert.equal(report.restored, false);
    assert.match(report.restoreError, /restore native Program and Preview/);
  } finally {
    await new Promise(resolve => server.close(resolve));
    await fs.rm(output, { recursive: true, force: true });
  }
});
