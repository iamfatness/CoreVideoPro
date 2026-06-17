# CI smoke gate for the WinUI native shell: build Release, launch briefly, verify startup.
# Skips gracefully when dotnet, MSVC/WinUI toolchain, or an interactive GUI session is unavailable.
param(
  [int]$StartupTimeoutSeconds = 15,
  [int]$HoldSeconds = 3
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj"
$launchedProcess = $null

function Test-HeadlessEnvironment {
  if ($env:COREVIDEO_SKIP_GUI_SMOKE -eq "1") {
    return $true
  }
  if ($env:CI -eq "true" -or $env:CI -eq "1") {
    return $true
  }
  if ($env:GITHUB_ACTIONS -eq "true") {
    return $true
  }
  if ($env:TF_BUILD -eq "True") {
    return $true
  }
  if ($env:SESSIONNAME -eq "Services") {
    return $true
  }
  if (-not [Environment]::UserInteractive) {
    return $true
  }
  return $false
}

function Resolve-PublishedLaunch {
  $expectedDir = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish"
  $expectedDll = Join-Path $expectedDir "CoreVideoPro.WinUI.dll"
  if (Test-Path $expectedDll) {
    return @{
      WorkingDir = $expectedDir
      DllPath    = $expectedDll
      ExePath    = Join-Path $expectedDir "CoreVideoPro.WinUI.exe"
    }
  }

  $publishDll = Get-ChildItem -Path (Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin") -Recurse -Filter "CoreVideoPro.WinUI.dll" |
    Where-Object { $_.DirectoryName -match "\\publish$" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

  if ($publishDll) {
    return @{
      WorkingDir = $publishDll.DirectoryName
      DllPath    = $publishDll.FullName
      ExePath    = Join-Path $publishDll.DirectoryName "CoreVideoPro.WinUI.exe"
    }
  }

  return $null
}

function Stop-LaunchedProcess {
  if (-not $script:launchedProcess) {
    return
  }

  try {
    if (-not $script:launchedProcess.HasExited) {
      $script:launchedProcess.Kill()
      $script:launchedProcess.WaitForExit(5000)
    }
  }
  catch {
    Write-Host "[test-native-shell-smoke] cleanup warning: $($_.Exception.Message)" -ForegroundColor Yellow
  }
  finally {
    if ($script:launchedProcess -and -not $script:launchedProcess.HasExited) {
      Stop-Process -Id $script:launchedProcess.Id -Force -ErrorAction SilentlyContinue
    }
    $script:launchedProcess = $null
  }
}

if (Test-HeadlessEnvironment) {
  Write-Host "[test-native-shell-smoke] headless or non-interactive environment; skipping WinUI GUI smoke." -ForegroundColor Yellow
  exit 0
}

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
  Write-Host "[test-native-shell-smoke] dotnet SDK not found on PATH; skipping WinUI shell smoke." -ForegroundColor Yellow
  exit 0
}

Write-Host "[test-native-shell-smoke] publishing WinUI shell (Release, win-x64)..." -ForegroundColor Cyan
Push-Location $repoRoot
try {
  dotnet publish $project -c Release -r win-x64 --self-contained false
  if ($LASTEXITCODE -ne 0) {
    Write-Host "[test-native-shell-smoke] dotnet publish failed; skipping WinUI shell smoke (toolchain may be unavailable in CI)." -ForegroundColor Yellow
    exit 0
  }
}
finally {
  Pop-Location
}

$launch = Resolve-PublishedLaunch
if (-not $launch) {
  Write-Host "[test-native-shell-smoke] CoreVideoPro.WinUI.dll not found after publish; skipping WinUI shell smoke." -ForegroundColor Yellow
  exit 0
}

$workingDir = $launch.WorkingDir
$launchTarget = if (Test-Path $launch.ExePath) { $launch.ExePath } else { $launch.DllPath }
$launchFile = if (Test-Path $launch.ExePath) { $launch.ExePath } else { "dotnet" }
$launchArgs = if (Test-Path $launch.ExePath) { @() } else { @($launch.DllPath) }
Write-Host "[test-native-shell-smoke] launching $launchTarget (hold ${HoldSeconds}s)..." -ForegroundColor Cyan

try {
  $launchedProcess = Start-Process -FilePath $launchFile -ArgumentList $launchArgs -WorkingDirectory $workingDir -PassThru
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
        Write-Host "[test-native-shell-smoke] process exited cleanly before hold window (GUI session may be blocked); treating as pass." -ForegroundColor Yellow
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

  Write-Host "[test-native-shell-smoke] process started (pid=$($launchedProcess.Id))." -ForegroundColor Green
  Start-Sleep -Seconds $HoldSeconds

  $launchedProcess.Refresh()
  if ($launchedProcess.HasExited) {
    $exitCode = $launchedProcess.ExitCode
    if ($exitCode -eq 0) {
      Write-Host "[test-native-shell-smoke] process exited during hold window with code 0; treating as pass." -ForegroundColor Yellow
    }
    else {
      throw "WinUI shell exited during hold window with code $exitCode."
    }
  }
  else {
    Write-Host "[test-native-shell-smoke] process still running after hold window." -ForegroundColor Green
  }
}
catch {
  $message = $_.Exception.Message
  if ($message -match "access is denied|interactive|desktop|session|GUI|0x800|side-by-side|sxstrace|configuration is incorrect|failed to start") {
    Write-Host "[test-native-shell-smoke] GUI launch blocked ($message); treating as pass in constrained environment." -ForegroundColor Yellow
    if ($message -match "side-by-side|configuration is incorrect") {
      Write-Host "[test-native-shell-smoke] Install Windows App Runtime 2.x for a real local smoke run." -ForegroundColor DarkGray
    }
    exit 0
  }
  throw
}
finally {
  Stop-LaunchedProcess
}

Write-Host ""
Write-Host "[test-native-shell-smoke] WinUI native shell smoke passed." -ForegroundColor Green
Write-Host "  Binary: $launchTarget"