import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('camera setup hashes without Get-FileHash and preserves ownership, ACLs and paths', {
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
  assert.match(result.stdout, /51 ownership\/path cases passed/);
});

test('camera setup validates and repairs the desktop user view, not the caller view', {
  skip: process.platform !== 'win32' ? 'Windows installer contract' : false,
}, () => {
  const env = { ...process.env };
  for (const key of Object.keys(env)) if (key.toLowerCase() === 'psmodulepath') delete env[key];
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', fileURLToPath(new URL('../alpha/Test-CameraUserSession.ps1', import.meta.url))],
  { encoding: 'utf8', timeout: 30000, env });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /desktop-user cases passed/);
});

test('independent Windows user worker returns scoped evidence over its private pipe', {
  skip: process.platform !== 'win32' || process.env.CI !== 'true' ? 'Clean Windows CI interactive-user boundary' : false,
}, () => {
  const env = { ...process.env };
  for (const key of Object.keys(env)) if (key.toLowerCase() === 'psmodulepath') delete env[key];
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', fileURLToPath(new URL('../alpha/Test-CameraUserSessionTransport.ps1', import.meta.url))],
  { encoding: 'utf8', timeout: 55000, env });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /Independent desktop-user transport passed/);
});
