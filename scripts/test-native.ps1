# CI gate for the portable C++ media-core stub (COREVIDEO_STUB=ON).
# Builds native binaries, runs unit tests, stages corevideo-native.exe to the
# paths the WinUI shell and desktop supervisor probe, and smoke-tests
# program-frame-preview parity on the built binary.
param(
  [string]$NativeDir = (Join-Path $PSScriptRoot "..\native"),
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build"),
  [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Test-CMakeAvailable {
  return [bool](Get-Command cmake -ErrorAction SilentlyContinue)
}

function Resolve-VsDevCmd {
  $candidates = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  )
  return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Quote-CmdArg([string]$Value) {
  if ($Value -match '[\s"]') {
    return '"' + ($Value -replace '"', '\"') + '"'
  }
  return $Value
}

function Invoke-NativeSmokeTest {
  param(
    [string]$Runner,
    [string[]]$RunnerArgs,
    [string]$Label
  )

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Runner
  $psi.Arguments = ($RunnerArgs | ForEach-Object { Quote-CmdArg $_ }) -join " "
  $psi.WorkingDirectory = $repoRoot
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true

  $process = [System.Diagnostics.Process]::Start($psi)
  if (-not $process) {
    throw "Failed to start $Label media core process."
  }

  function Send-Line([string]$json) {
    $process.StandardInput.WriteLine($json)
    $process.StandardInput.Flush()
    return $process.StandardOutput.ReadLine()
  }

  try {
    $handshake = Send-Line '{"id":"native-1","type":"handshake"}' | ConvertFrom-Json
    if (-not $handshake.ok) { throw "$Label handshake failed." }

    $syncPayload = @{
      id = "native-2"
      type = "media-core-sync"
      elapsedMs = 1000
      commands = @(
        @{
          type = "load-scene-graph"
          sceneId = "preview-test"
          routes = @(
            @{
              routeId = "program"
              mode = "fixed"
              audioRole = "mix"
              participantId = "speaker-1"
            }
          )
        },
        @{
          type = "start-program-output"
          destinations = @("recording")
          isoParticipantIds = @()
        }
      )
    } | ConvertTo-Json -Depth 8 -Compress

    $syncLine = Send-Line $syncPayload
    $sync = $syncLine | ConvertFrom-Json
    if (-not $sync.ok) { throw "$Label media-core-sync failed." }

    $embeddedPreview = $null
    if ($sync.snapshot -and $sync.snapshot.programFramePreview) {
      $embeddedPreview = $sync.snapshot.programFramePreview
    }

    $previewEvent = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
      $line = $process.StandardOutput.ReadLine()
      if (-not $line) {
        Start-Sleep -Milliseconds 50
        continue
      }
      try {
        $parsed = $line | ConvertFrom-Json
      }
      catch {
        continue
      }
      if ($parsed.type -eq "program-frame-preview") {
        $previewEvent = $parsed
        break
      }
    }

    if (-not $previewEvent -and $embeddedPreview) {
      $previewEvent = [pscustomobject]@{
        type = "program-frame-preview"
        preview = $embeddedPreview
      }
    }
    if (-not $previewEvent) {
      throw "$Label did not emit program-frame-preview event."
    }
    if ($previewEvent.preview.pixelFormat -ne "bgra") {
      throw "$Label preview pixelFormat must be bgra."
    }
    if ([string]::IsNullOrWhiteSpace($previewEvent.preview.bgraBase64)) {
      throw "$Label preview bgraBase64 must be non-empty."
    }
    if ($previewEvent.preview.width -gt 320 -or $previewEvent.preview.height -gt 180) {
      throw "$Label preview exceeds 320x180 downscale cap."
    }

    Write-Host "[test-native] $Label program-frame-preview smoke ok ($($previewEvent.preview.width)x$($previewEvent.preview.height))" -ForegroundColor Green
  }
  finally {
    if (-not $process.HasExited) {
      $process.Kill()
      $process.WaitForExit()
    }
  }
}

function Stage-NativeArtifacts {
  param(
    [string]$SourceDir
  )

  $targets = @(
    (Join-Path $NativeDir "build"),
    (Join-Path $NativeDir "build-dev"),
    (Join-Path $NativeDir "build-dev\$Config")
  )

  foreach ($targetDir in $targets) {
    if (-not (Test-Path $targetDir)) {
      New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }
    foreach ($name in @("corevideo-native.exe", "corevideo-native-tests.exe")) {
      $source = Join-Path $SourceDir $name
      $destination = Join-Path $targetDir $name
      if (-not (Test-Path $source)) {
        continue
      }
      $resolvedDestination = Resolve-Path $destination -ErrorAction SilentlyContinue
      if ($resolvedDestination -and (Resolve-Path $source).Path -ieq $resolvedDestination.Path) {
        continue
      }
      Copy-Item -Path $source -Destination $destination -Force
      Write-Host "[test-native] staged $name -> $targetDir" -ForegroundColor DarkGray
    }
  }
}

if (-not (Test-CMakeAvailable)) {
  Write-Host "[test-native] cmake not found on PATH; skipping native media-core CI gate." -ForegroundColor Yellow
  exit 0
}

$vsDevCmd = Resolve-VsDevCmd
if (-not $vsDevCmd) {
  Write-Host "[test-native] Visual Studio / Build Tools not found; skipping native media-core CI gate." -ForegroundColor Yellow
  exit 0
}

Write-Host "[test-native] configuring portable stub build (COREVIDEO_STUB=ON)..." -ForegroundColor Cyan
$cmakeConfigureArgs = @(
  "-S", $NativeDir,
  "-B", $BuildDir,
  "-DCOREVIDEO_STUB=ON",
  "-DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF",
  "-DBUILD_TESTING=ON"
)
$cmakeBuildArgs = @("--build", $BuildDir, "--config", $Config, "--target", "corevideo-native", "corevideo-native-tests")

$configureCmd = @(
  "call `"$vsDevCmd`" -arch=amd64",
  "cmake $(($cmakeConfigureArgs | ForEach-Object { Quote-CmdArg $_ }) -join ' ')"
) -join " && "
cmd /c $configureCmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildCmd = @(
  "call `"$vsDevCmd`" -arch=amd64",
  "cmake $(($cmakeBuildArgs | ForEach-Object { Quote-CmdArg $_ }) -join ' ')"
) -join " && "
cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$artifactCandidates = @(
  (Join-Path $BuildDir $Config),
  $BuildDir
)
$artifactDir = $artifactCandidates | Where-Object { Test-Path (Join-Path $_ "corevideo-native.exe") } | Select-Object -First 1
if (-not $artifactDir) {
  throw "corevideo-native.exe not found under $BuildDir"
}

$testBinary = Join-Path $artifactDir "corevideo-native-tests.exe"
if (-not (Test-Path $testBinary)) {
  throw "Native test binary not found at $testBinary"
}

Write-Host "[test-native] running native unit tests..." -ForegroundColor Cyan
Push-Location $artifactDir
try {
  if (Get-Command ctest -ErrorAction SilentlyContinue) {
    ctest -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  }
  else {
    & $testBinary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  }
}
finally {
  Pop-Location
}

Stage-NativeArtifacts -SourceDir $artifactDir

Write-Host "[test-native] bundling desktop/coreStub.cjs..." -ForegroundColor Cyan
Push-Location $repoRoot
try {
  node --input-type=module -e "import { ensureCoreStubBundle } from './desktop/scripts/desktopRuntime.mjs'; ensureCoreStubBundle();"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
  Pop-Location
}

$nativeExe = Join-Path $artifactDir "corevideo-native.exe"
if (-not (Test-Path $nativeExe)) {
  throw "corevideo-native.exe not found at $nativeExe"
}

Invoke-NativeSmokeTest -Runner $nativeExe -RunnerArgs @() -Label "native"
Invoke-NativeSmokeTest -Runner "node" -RunnerArgs @((Join-Path $repoRoot "desktop\coreStub.cjs")) -Label "coreStub"

Write-Host ""
Write-Host "[test-native] native media-core CI gate passed." -ForegroundColor Green
Write-Host "  Build:  $artifactDir"
Write-Host "  Binary: $nativeExe"
Write-Host "  Tests:  $testBinary"