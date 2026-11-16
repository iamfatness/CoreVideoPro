import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Review M4 (2026-11-15): the e2e script copies the signed vcam probe, an
// admitted producer carrying get-task-allow, into the world-writable
// /Users/Shared. Its EXIT trap must delete it. This runs the script's own
// cleanup() against a scratch file, with collect_logs stubbed.
test('macos-vcam-e2e cleanup() deletes the probe it planted', {
  skip: process.platform === 'win32' ? 'bash script, macOS e2e' : false,
}, () => {
  const script = readFileSync(fileURLToPath(new URL('../macos-vcam-e2e.sh', import.meta.url)), 'utf8');
  const match = script.match(/^cleanup\(\) \{\n[\s\S]*?^\}\n/m);
  assert.ok(match, 'cleanup() not found in macos-vcam-e2e.sh');
  const dir = mkdtempSync(join(tmpdir(), 'vcam-e2e-cleanup-'));
  try {
    const feed = join(dir, 'corevideo-vcam-feed');
    writeFileSync(feed, 'probe');
    const harness = `collect_logs() { :; }\nFEED_PID=""\nFEED=${JSON.stringify(feed)}\nOUT=${JSON.stringify(dir)}\n${match[0]}cleanup\n`;
    const result = spawnSync('bash', ['-c', harness], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.equal(existsSync(feed), false, 'cleanup() left the probe behind');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
