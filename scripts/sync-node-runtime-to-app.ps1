# Stage a Node.js runtime + the built show-engine host beside the packaged app.
# The show-engine host (`show-engine/dist/host/main.js`) has NO runtime npm
# dependencies, so this only needs to copy `show-engine/dist/**` +
# `show-engine/package.json` and a `node.exe` that satisfies `engines.node`
# (>=24) alongside it. `ShowEnginePaths` (Task 9) probes
# `<AppDir>\node\node.exe` + `<AppDir>\show-engine\dist\host\main.js`.
param(
  [Parameter(Mandatory = $true)]
  [string]$AppDir,
  [string]$NodeExe = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$showEngineDir = Join-Path $repoRoot "show-engine"

function Write-NodeRuntimeManifest {
  param(
    [string]$NodeVersion,
    [string]$Entry,
    [string]$NodeSource
  )

  $manifest = @{
    nodeVersion = $NodeVersion
    stagedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    entry = $Entry
    nodeSource = $NodeSource
  } | ConvertTo-Json -Depth 4

  $manifestPath = Join-Path $AppDir "corevideo-show-engine-runtime.json"
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $resolvedManifestPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($manifestPath)
  [System.IO.File]::WriteAllText($resolvedManifestPath, $manifest, $utf8NoBom)
}

function Resolve-NodeExe {
  param([string]$Override)

  if ($Override -and (Test-Path $Override)) {
    return (Resolve-Path $Override).ProviderPath
  }

  if ($env:COREVIDEO_NODE_EXE -and (Test-Path $env:COREVIDEO_NODE_EXE)) {
    return (Resolve-Path $env:COREVIDEO_NODE_EXE).ProviderPath
  }

  $node = Get-Command node -ErrorAction SilentlyContinue
  if ($node) {
    return $node.Source
  }

  return $null
}

if (-not (Test-Path $AppDir)) {
  New-Item -ItemType Directory -Path $AppDir -Force | Out-Null
}

$nodeSource = Resolve-NodeExe -Override $NodeExe
if (-not $nodeSource) {
  throw "node.exe not found. Pass -NodeExe, set COREVIDEO_NODE_EXE, or ensure node is on PATH."
}

$versionOutput = & $nodeSource --version
if ($LASTEXITCODE -ne 0) {
  throw "Failed to run '$nodeSource --version' (exit $LASTEXITCODE)."
}
$versionString = $versionOutput.Trim()
if ($versionString -notmatch '^v(\d+)\.') {
  throw "Unable to parse Node version from '$versionString'."
}
$majorVersion = [int]$Matches[1]
if ($majorVersion -lt 24) {
  throw "show-engine requires Node 24+, found $versionString"
}

$distHostMain = Join-Path $showEngineDir "dist\host\main.js"
if (-not (Test-Path $distHostMain)) {
  throw "show-engine/dist/host/main.js is missing - run npm run build:show-engine first"
}

Write-Host "[node-runtime] staging node $versionString runtime beside packaged app..." -ForegroundColor Cyan

$nodeOutDir = Join-Path $AppDir "node"
if (-not (Test-Path $nodeOutDir)) {
  New-Item -ItemType Directory -Path $nodeOutDir -Force | Out-Null
}
Copy-Item -Path $nodeSource -Destination (Join-Path $nodeOutDir "node.exe") -Force
Write-Host "[node-runtime] staged node.exe from $nodeSource" -ForegroundColor DarkGray

$showEngineOutDir = Join-Path $AppDir "show-engine"
$distOutDir = Join-Path $showEngineOutDir "dist"
if (Test-Path $distOutDir) {
  Remove-Item $distOutDir -Recurse -Force
}
New-Item -ItemType Directory -Path $distOutDir -Force | Out-Null
Copy-Item -Path (Join-Path $showEngineDir "dist\*") -Destination $distOutDir -Recurse -Force
Copy-Item -Path (Join-Path $showEngineDir "package.json") -Destination (Join-Path $showEngineOutDir "package.json") -Force
Write-Host "[node-runtime] staged show-engine/dist/** + package.json" -ForegroundColor DarkGray

Write-NodeRuntimeManifest -NodeVersion $versionString -Entry "show-engine\dist\host\main.js" -NodeSource $nodeSource

Write-Host "[node-runtime] staged show-engine host runtime ($versionString) into $AppDir" -ForegroundColor DarkGray
