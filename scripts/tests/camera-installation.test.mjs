import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('camera setup ownership, rollback names, protected ACLs, and path guards', {
  skip: process.platform !== 'win32' ? 'Windows registry/ACL contract' : false,
}, () => {
  // A PowerShell 7 parent can export PSModulePath entries incompatible with
  // Windows PowerShell 5.1, which the installed helper uses.
  const env = { ...process.env };
  for (const key of Object.keys(env)) if (key.toLowerCase() === 'psmodulepath') delete env[key];
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', fileURLToPath(new URL('../alpha/Test-VirtualCameraOwnership.ps1', import.meta.url))],
  { encoding: 'utf8', timeout: 30000, env });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /22 ownership\/path cases passed/);
});
