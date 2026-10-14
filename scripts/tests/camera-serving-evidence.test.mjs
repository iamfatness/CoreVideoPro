import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('camera qualification rejects stale or unobserved serving identity', () => {
  const result = spawnSync(process.platform === 'win32' ? 'python' : 'python3',
    [fileURLToPath(new URL('../qa/test_camera_serving_evidence.py', import.meta.url))],
    { encoding: 'utf8', timeout: 30000 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
});
