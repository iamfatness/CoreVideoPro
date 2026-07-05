# Audio soak harness (zoom-audio-spec.md Z4b): swap in the fake engine's
# deterministic tone streams, run the FULL audio pipeline for N minutes, scan
# the MON bus tap for clicks/gaps/level-wobble/echo, print a machine verdict.
# The real engine binaries are ALWAYS restored (finally), even on failure.
#
#   powershell -File tools/audio/soak.ps1 [-Minutes 3] [-KeepApp]
#
# Prereqs: native dev build present (npm run build:native-dev), Node on PATH.
param(
    [int]$Minutes = 3,
    [switch]$KeepApp
)
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$tapDir = Join-Path $repoRoot "audiotap-soak"
$publish = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish"
$enginePaths = @(
    (Join-Path $repoRoot "native\build-dev\corevideo-zoom-engine.exe"),
    (Join-Path $repoRoot "native\build-dev\Release\corevideo-zoom-engine.exe"),
    (Join-Path $publish "corevideo-zoom-engine.exe")
) | Where-Object { Test-Path $_ }
$fake = Join-Path $repoRoot "native\build-dev\corevideo-zoom-engine-fake.exe"
if (-not (Test-Path $fake)) { throw "fake engine not built: $fake" }

function Stop-App {
    Get-Process -Name "CoreVideoPro.WinUI","corevideo-native","corevideo-zoom-engine" -ErrorAction SilentlyContinue |
        Stop-Process -Force -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

Write-Host "[soak] backing up real engine + staging fake tone engine" -ForegroundColor Cyan
Stop-App
$backups = @{}
foreach ($path in $enginePaths) {
    $bak = "$path.realbak"
    Copy-Item $path $bak -Force
    $backups[$path] = $bak
    Copy-Item $fake $path -Force
}

try {
    New-Item -ItemType Directory -Force $tapDir | Out-Null
    Get-ChildItem $tapDir | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -Confirm:$false }
    $env:COREVIDEO_AUDIO_DEBUG_DIR = $tapDir

    Write-Host "[soak] launching app" -ForegroundColor Cyan
    Push-Location $repoRoot
    npm run app -- -StubOnly 2>&1 | Select-String "running \(pid" | ForEach-Object { Write-Host $_.Line }
    Pop-Location
    Start-Sleep -Seconds 12

    Write-Host "[soak] driving Zoom join via UIA (fake engine accepts any id)" -ForegroundColor Cyan
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $app = Get-Process -Name "CoreVideoPro.WinUI" | Select-Object -First 1
    if (-not $app) { throw "app did not start" }
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($app.MainWindowHandle)
    $nav = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, "Zoom")))
    if ($nav) { ($nav.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke() }
    Start-Sleep -Seconds 2
    $urlBox = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, "Zoom meeting URL or ID")))
    if ($urlBox) { ($urlBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)).SetValue("1234567890") }
    $join = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, "Join Zoom")))
    if ($join) { ($join.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)).Invoke() }

    Write-Host "[soak] capturing $Minutes minute(s) of tone through the full pipeline..." -ForegroundColor Cyan
    Start-Sleep -Seconds ($Minutes * 60)

    $monTap = Join-Path $tapDir "tap-mon.f32"
    if (-not (Test-Path $monTap) -or (Get-Item $monTap).Length -lt 5MB) {
        Write-Host "[soak] FAIL: MON tap missing/too small - tone never reached the mix (join failed? routing?)" -ForegroundColor Red
        Get-ChildItem $tapDir | ForEach-Object { Write-Host "  $($_.Name) $($_.Length)" }
        exit 1
    }
    Write-Host "[soak] scanning $((Get-Item $monTap).Length / 1MB -as [int])MB MON tap" -ForegroundColor Cyan
    node (Join-Path $PSScriptRoot "tone-scan.js") $monTap
    $verdict = $LASTEXITCODE
    exit $verdict
}
finally {
    Write-Host "[soak] restoring real engine binaries" -ForegroundColor Cyan
    if (-not $KeepApp) { Stop-App }
    foreach ($path in $backups.Keys) {
        Copy-Item $backups[$path] $path -Force
        Remove-Item $backups[$path] -Force -Confirm:$false
    }
    Remove-Item Env:\COREVIDEO_AUDIO_DEBUG_DIR -ErrorAction SilentlyContinue
}
