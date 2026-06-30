# CI gate for the portable C++ media-core stub (COREVIDEO_STUB=ON).
# Builds native binaries, runs unit tests, stages corevideo-native.exe to the
# paths the native shells probe, and smoke-tests
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
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2026\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  )
  $candidate = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($candidate) {
    return $candidate
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $discovered = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "Common7\Tools\VsDevCmd.bat" | Select-Object -First 1
    if ($discovered -and (Test-Path $discovered)) {
      return $discovered
    }
  }

  return $null
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

  & node (Join-Path $repoRoot "scripts\native-stdio-smoke.mjs") $Label $Runner @RunnerArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

function Stage-NativeArtifacts {
  param(
    [string]$SourceDir
  )

  # Stage only CI stub probe paths. build-dev/ is owned by scripts/build-native-dev.ps1
  # (D3D11 + dev adapters) and must not be overwritten by the portable stub gate.
  $targets = @(
    (Join-Path $NativeDir "build"),
    (Join-Path $NativeDir "build\$Config")
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
  "-DCOREVIDEO_WITH_ZOOM=OFF",
  "-DCOREVIDEO_WITH_D3D11=OFF",
  "-DCOREVIDEO_WITH_MF_ENCODER=OFF",
  "-DCOREVIDEO_WITH_WASAPI_MONITOR=OFF",
  "-DCOREVIDEO_WITH_WASAPI_CAPTURE=OFF",
  "-DCOREVIDEO_WITH_RTMP_OUTPUT=OFF",
  "-DCOREVIDEO_WITH_SRT_OUTPUT=OFF",
  "-DCOREVIDEO_WITH_NDI_OUTPUT=OFF",
  "-DCOREVIDEO_WITH_SRT_INGEST=OFF",
  "-DCOREVIDEO_WITH_DECKLINK=OFF",
  "-DCOREVIDEO_WITH_AJA=OFF",
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
) | Where-Object { Test-Path (Join-Path $_ "corevideo-native.exe") }
if (-not $artifactCandidates -or $artifactCandidates.Count -eq 0) {
  throw "corevideo-native.exe not found under $BuildDir"
}
$artifactDir = $artifactCandidates |
  Sort-Object { (Get-Item (Join-Path $_ "corevideo-native.exe")).LastWriteTimeUtc } -Descending |
  Select-Object -First 1

$testBinary = Join-Path $artifactDir "corevideo-native-tests.exe"
if (-not (Test-Path $testBinary)) {
  throw "Native test binary not found at $testBinary"
}

Write-Host "[test-native] running native unit tests..." -ForegroundColor Cyan
Push-Location $BuildDir
try {
  if (Get-Command ctest -ErrorAction SilentlyContinue) {
    ctest -C $Config --output-on-failure --test-dir $BuildDir
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

$nativeExe = Join-Path $artifactDir "corevideo-native.exe"
if (-not (Test-Path $nativeExe)) {
  throw "corevideo-native.exe not found at $nativeExe"
}

Invoke-NativeSmokeTest -Runner $nativeExe -RunnerArgs @() -Label "native"

Write-Host ""
Write-Host "[test-native] native media-core CI gate passed." -ForegroundColor Green
Write-Host "  Build:  $artifactDir"
Write-Host "  Binary: $nativeExe"
Write-Host "  Tests:  $testBinary"
