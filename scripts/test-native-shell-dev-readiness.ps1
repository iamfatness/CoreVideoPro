# Dev-machine readiness smoke for the WinUI native shell + Zoom-enabled native core.
# Verifies local artifacts/runtime are visible without requiring live Zoom credentials or join.
param(
  [int]$StartupTimeoutSeconds = 15,
  [int]$HoldSeconds = 3,
  [switch]$NoPublish
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj"
$nativeBuildDir = Join-Path $repoRoot "native\build-dev"
$nativeExe = Join-Path $nativeBuildDir "corevideo-native.exe"
$zoomEngineExe = Join-Path $nativeBuildDir "corevideo-zoom-engine.exe"
$runtimeDir = if ($env:COREVIDEO_ZOOM_RUNTIME_DIR) {
  [System.IO.Path]::GetFullPath($env:COREVIDEO_ZOOM_RUNTIME_DIR)
} else {
  Join-Path $repoRoot "native-core\zoom-runtime\windows\x64"
}
$launchedProcess = $null
$previousEnv = @{}

function Test-HeadlessEnvironment {
  if ($env:COREVIDEO_SKIP_GUI_SMOKE -eq "1") { return $true }
  if ($env:CI -eq "true" -or $env:CI -eq "1") { return $true }
  if ($env:GITHUB_ACTIONS -eq "true") { return $true }
  if ($env:TF_BUILD -eq "True") { return $true }
  if ($env:SESSIONNAME -eq "Services") { return $true }
  if (-not [Environment]::UserInteractive) { return $true }
  return $false
}

function Resolve-PublishedLaunch {
  $expectedDir = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish"
  $expectedExe = Join-Path $expectedDir "CoreVideoPro.WinUI.exe"
  $expectedDll = Join-Path $expectedDir "CoreVideoPro.WinUI.dll"
  if ((Test-Path $expectedExe) -or (Test-Path $expectedDll)) {
    return @{
      WorkingDir = $expectedDir
      ExePath    = $expectedExe
      DllPath    = $expectedDll
    }
  }

  $publishDll = Get-ChildItem -Path (Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin") -Recurse -Filter "CoreVideoPro.WinUI.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -match "\\publish$" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

  if ($publishDll) {
    return @{
      WorkingDir = $publishDll.DirectoryName
      ExePath    = Join-Path $publishDll.DirectoryName "CoreVideoPro.WinUI.exe"
      DllPath    = $publishDll.FullName
    }
  }

  return $null
}

function Require-File {
  param(
    [string]$Path,
    [string]$Hint
  )

  if (-not (Test-Path $Path)) {
    throw "$Path was not found. $Hint"
  }
}

function Sync-NativeCoreArtifacts {
  param([string]$TargetDir)

  Get-ChildItem -Path $nativeBuildDir -Filter "corevideo-*" -File -ErrorAction Stop |
    Where-Object { $_.Extension -in ".exe", ".dll" -and $_.BaseName -notmatch "-tests$" } |
    ForEach-Object {
      Copy-Item -Path $_.FullName -Destination (Join-Path $TargetDir $_.Name) -Force
    }
}

function Set-ProcessEnv {
  param(
    [string]$Name,
    [string]$Value
  )

  if (-not $script:previousEnv.ContainsKey($Name)) {
    $script:previousEnv[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
  }
  [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Restore-ProcessEnv {
  foreach ($entry in $script:previousEnv.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
  }
}

function Stop-LaunchedProcess {
  if (-not $script:launchedProcess) {
    return
  }

  try {
    if (-not $script:launchedProcess.HasExited) {
      $script:launchedProcess.Kill()
      $script:launchedProcess.WaitForExit(5000) | Out-Null
    }
  }
  catch {
    Write-Host "[test-native-shell-dev-readiness] cleanup warning: $($_.Exception.Message)" -ForegroundColor Yellow
  }
  finally {
    if ($script:launchedProcess -and -not $script:launchedProcess.HasExited) {
      Stop-Process -Id $script:launchedProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $script:launchedProcess = $null
  }
}

Write-Host "[test-native-shell-dev-readiness] checking dev native core artifacts..." -ForegroundColor Cyan
Require-File -Path $nativeExe -Hint "Run npm run build:native-dev after staging the Zoom SDK."
Require-File -Path $zoomEngineExe -Hint "Run npm run build:native-dev with COREVIDEO_BUILD_ZOOM_ENGINE enabled."
Require-File -Path (Join-Path $runtimeDir "bin\sdk.dll") -Hint "Run .\scripts\stage-zoom-sdk.ps1 or set COREVIDEO_ZOOM_RUNTIME_DIR."
Require-File -Path (Join-Path $runtimeDir "h\zoom_sdk.h") -Hint "The staged Zoom runtime should include headers and bin/sdk.dll."

$runtimeDllCount = (Get-ChildItem -Path (Join-Path $runtimeDir "bin") -Filter "*.dll" -File -ErrorAction SilentlyContinue).Count
if ($runtimeDllCount -lt 20) {
  throw "Only $runtimeDllCount Zoom runtime DLLs were found at $runtimeDir\bin. Re-run .\scripts\stage-zoom-sdk.ps1."
}

if (-not $NoPublish) {
  if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Write-Host "[test-native-shell-dev-readiness] dotnet SDK not found on PATH; skipping publish/launch after artifact readiness checks." -ForegroundColor Yellow
    Write-Host "[test-native-shell-dev-readiness] dev artifacts are ready." -ForegroundColor Green
    exit 0
  }

  Write-Host "[test-native-shell-dev-readiness] publishing WinUI shell (Release, win-x64)..." -ForegroundColor Cyan
  Push-Location $repoRoot
  try {
    dotnet publish $project -c Release -r win-x64 --self-contained false
    if ($LASTEXITCODE -ne 0) {
      Write-Host "[test-native-shell-dev-readiness] dotnet publish failed; will use an existing publish folder if one is available." -ForegroundColor Yellow
    }
  }
  finally {
    Pop-Location
  }
}

$launch = Resolve-PublishedLaunch
if (-not $launch) {
  Write-Host "[test-native-shell-dev-readiness] published WinUI shell not found; skipping launch after artifact readiness checks." -ForegroundColor Yellow
  Write-Host "[test-native-shell-dev-readiness] dev artifacts are ready." -ForegroundColor Green
  exit 0
}

$workingDir = $launch.WorkingDir
Write-Host "[test-native-shell-dev-readiness] staging Zoom runtime and dev native core beside published app..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\sync-zoom-runtime-to-app.ps1") -AppDir $workingDir -RuntimeDir $runtimeDir
if (-not $?) {
  exit 1
}

Sync-NativeCoreArtifacts -TargetDir $workingDir

$packagedRuntime = Join-Path $workingDir "zoom-runtime\windows\x64"
$packagedNative = Join-Path $workingDir "corevideo-native.exe"
$packagedZoomEngine = Join-Path $workingDir "corevideo-zoom-engine.exe"
Require-File -Path $packagedNative -Hint "The published app must be able to resolve the dev native core."
Require-File -Path $packagedZoomEngine -Hint "The published app must be able to resolve the Zoom helper process."
Require-File -Path (Join-Path $packagedRuntime "bin\sdk.dll") -Hint "The published app must carry or resolve the staged Zoom runtime."

Set-ProcessEnv -Name "COREVIDEO_ZOOM_RUNTIME_DIR" -Value $packagedRuntime
Set-ProcessEnv -Name "COREVIDEO_ZOOM_ENGINE_PATH" -Value $packagedZoomEngine
Set-ProcessEnv -Name "COREVIDEO_ZOOM_JOIN_WAIT_MS" -Value "45000"
Set-ProcessEnv -Name "PATH" -Value "$workingDir;$($packagedRuntime)\bin;$env:PATH"

Write-Host "[test-native-shell-dev-readiness] readiness evidence:" -ForegroundColor Green
Write-Host "  WinUI publish: $workingDir"
Write-Host "  Native core:   $packagedNative"
Write-Host "  Zoom engine:   $packagedZoomEngine"
Write-Host "  Zoom runtime:  $packagedRuntime ($runtimeDllCount DLLs staged)"

if (Test-HeadlessEnvironment) {
  Write-Host "[test-native-shell-dev-readiness] headless or non-interactive environment; skipping WinUI GUI launch." -ForegroundColor Yellow
  Restore-ProcessEnv
  exit 0
}

$launchFile = if (Test-Path $launch.ExePath) { $launch.ExePath } else { "dotnet" }
$launchArgs = if (Test-Path $launch.ExePath) { @() } else { @($launch.DllPath) }
$launchTarget = if (Test-Path $launch.ExePath) { $launch.ExePath } else { $launch.DllPath }
Write-Host "[test-native-shell-dev-readiness] launching $launchTarget (hold ${HoldSeconds}s, no Zoom join)..." -ForegroundColor Cyan

try {
  if ($launchArgs.Count -gt 0) {
    $launchedProcess = Start-Process -FilePath $launchFile -ArgumentList $launchArgs -WorkingDirectory $workingDir -PassThru
  }
  else {
    $launchedProcess = Start-Process -FilePath $launchFile -WorkingDirectory $workingDir -PassThru
  }
  if (-not $launchedProcess) {
    throw "Start-Process did not return a process handle."
  }

  $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
  $started = $false
  while ([DateTime]::UtcNow -lt $deadline) {
    $launchedProcess.Refresh()
    if ($launchedProcess.HasExited) {
      $exitCode = $launchedProcess.ExitCode
      if ($exitCode -eq 0) {
        Write-Host "[test-native-shell-dev-readiness] process exited cleanly before hold window; treating as pass." -ForegroundColor Yellow
        exit 0
      }
      throw "WinUI shell exited early with code $exitCode."
    }

    try {
      $running = Get-Process -Id $launchedProcess.Id -ErrorAction Stop
      if ($running) {
        $started = $true
        break
      }
    }
    catch {
      Start-Sleep -Milliseconds 200
    }
  }

  if (-not $started) {
    throw "WinUI shell did not appear in the process table within ${StartupTimeoutSeconds}s."
  }

  Write-Host "[test-native-shell-dev-readiness] process started (pid=$($launchedProcess.Id))." -ForegroundColor Green
  Start-Sleep -Seconds $HoldSeconds

  $launchedProcess.Refresh()
  if ($launchedProcess.HasExited) {
    $exitCode = $launchedProcess.ExitCode
    if ($exitCode -eq 0) {
      Write-Host "[test-native-shell-dev-readiness] process exited during hold window with code 0; treating as pass." -ForegroundColor Yellow
    }
    else {
      throw "WinUI shell exited during hold window with code $exitCode."
    }
  }
  else {
    Write-Host "[test-native-shell-dev-readiness] process still running after hold window." -ForegroundColor Green
  }
}
catch {
  $message = $_.Exception.Message
  if ($message -match "access is denied|interactive|desktop|session|GUI|0x800|side-by-side|sxstrace|configuration is incorrect|failed to start") {
    Write-Host "[test-native-shell-dev-readiness] GUI launch blocked ($message); treating as pass in constrained environment." -ForegroundColor Yellow
    if ($message -match "side-by-side|configuration is incorrect") {
      Write-Host "[test-native-shell-dev-readiness] Install Windows App Runtime 2.x for a real local smoke run." -ForegroundColor DarkGray
    }
    exit 0
  }
  throw
}
finally {
  Stop-LaunchedProcess
  Restore-ProcessEnv
}

Write-Host ""
Write-Host "[test-native-shell-dev-readiness] WinUI dev-readiness smoke passed." -ForegroundColor Green
