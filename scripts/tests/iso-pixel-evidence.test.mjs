import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('ISO pixel proof rejects omitted, repeated, stale-owner and low-resolution evidence', () => {
  const result = spawnSync(process.env.COREVIDEO_TEST_PYTHON ?? (process.platform === 'win32' ? 'python' : 'python3'),
    [fileURLToPath(new URL('../qa/test_iso_pixels.py', import.meta.url))],
    { encoding: 'utf8', timeout: 30000 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
});
