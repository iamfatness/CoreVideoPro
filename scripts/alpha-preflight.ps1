param(
  [switch]$SkipWinUIBuild,
  [switch]$SkipNativeBuild,
  [switch]$SkipRecordStream,
  [switch]$SkipDevReadiness,
  [string]$OutputDir
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$artifactRoot = if ([string]::IsNullOrWhiteSpace($OutputDir)) {
  Join-Path $repoRoot "artifacts\alpha-preflight\$timestamp"
} else {
  [System.IO.Path]::GetFullPath($OutputDir)
}
$logDir = Join-Path $artifactRoot "logs"
$reportPath = Join-Path $artifactRoot "alpha-preflight.md"
$jsonPath = Join-Path $artifactRoot "alpha-preflight.json"
$results = New-Object System.Collections.Generic.List[object]

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Add-Result {
  param(
    [string]$Name,
    [string]$Status,
    [int]$ExitCode,
    [string]$LogPath,
    [string]$Notes
  )

  $script:results.Add([pscustomobject]@{
    name = $Name
    status = $Status
    exitCode = $ExitCode
    logPath = $LogPath
    notes = $Notes
  })
}

function Invoke-AlphaStep {
  param(
    [string]$Name,
    [string]$Command,
    [switch]$AllowFailure
  )

  $safeName = ($Name -replace "[^a-zA-Z0-9_.-]", "-").ToLowerInvariant()
  $logPath = Join-Path $logDir "$safeName.log"
  $started = Get-Date
  Write-Host "[alpha-preflight] $Name" -ForegroundColor Cyan
  Push-Location $repoRoot
  try {
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -Command $Command 2>&1
    $exitCode = $LASTEXITCODE
    $ended = Get-Date
    @(
      "Command: $Command"
      "Started: $($started.ToString("o"))"
      "Ended:   $($ended.ToString("o"))"
      "Exit:    $exitCode"
      ""
      $output
    ) | Out-File -FilePath $logPath -Encoding utf8

    if ($exitCode -eq 0) {
      Add-Result -Name $Name -Status "passed" -ExitCode $exitCode -LogPath $logPath -Notes ""
      Write-Host "[alpha-preflight] passed: $Name" -ForegroundColor Green
      return
    }

    $status = if ($AllowFailure) { "warning" } else { "failed" }
    Add-Result -Name $Name -Status $status -ExitCode $exitCode -LogPath $logPath -Notes "See log for command output."
    Write-Host "[alpha-preflight] ${status}: $Name (exit $exitCode)" -ForegroundColor Yellow
  }
  catch {
    $message = $_.Exception.Message
    "Command: $Command`nException: $message" | Out-File -FilePath $logPath -Encoding utf8
    $status = if ($AllowFailure) { "warning" } else { "failed" }
    Add-Result -Name $Name -Status $status -ExitCode -1 -LogPath $logPath -Notes $message
    Write-Host "[alpha-preflight] ${status}: $Name ($message)" -ForegroundColor Yellow
  }
  finally {
    Pop-Location
  }
}

function Test-PathEvidence {
  param(
    [string]$Name,
    [string]$Path,
    [string]$PassNotes,
    [string]$FailNotes
  )

  if (Test-Path $Path) {
    Add-Result -Name $Name -Status "passed" -ExitCode 0 -LogPath "" -Notes $PassNotes
    Write-Host "[alpha-preflight] passed: $Name" -ForegroundColor Green
  }
  else {
    Add-Result -Name $Name -Status "warning" -ExitCode 1 -LogPath "" -Notes $FailNotes
    Write-Host "[alpha-preflight] warning: $Name" -ForegroundColor Yellow
  }
}

$head = git rev-parse --short HEAD
$branch = git branch --show-current
$status = git status --short

Invoke-AlphaStep -Name "npm audit" -Command "npm audit --audit-level=moderate"
Invoke-AlphaStep -Name "TypeScript typecheck" -Command "npm run typecheck"
Invoke-AlphaStep -Name "MediaCore tests" -Command "dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj --no-restore -p:NuGetAudit=false"
Invoke-AlphaStep -Name "WinUI view-model tests" -Command "dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj --no-restore -p:NuGetAudit=false"

if (-not $SkipNativeBuild) {
  Invoke-AlphaStep -Name "Native media-core stub build and smoke" -Command "npm run test:native-media-core"
}
else {
  Add-Result -Name "Native media-core stub build and smoke" -Status "skipped" -ExitCode 0 -LogPath "" -Notes "Skipped by parameter."
}

if (-not $SkipWinUIBuild) {
  Invoke-AlphaStep -Name "WinUI build" -Command "dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -p:NuGetAudit=false"
}
else {
  Add-Result -Name "WinUI build" -Status "skipped" -ExitCode 0 -LogPath "" -Notes "Skipped by parameter."
}

$devNative = Join-Path $repoRoot "native\build-dev\corevideo-native.exe"
$devZoomEngine = Join-Path $repoRoot "native\build-dev\corevideo-zoom-engine.exe"
$zoomRuntime = if ($env:COREVIDEO_ZOOM_RUNTIME_DIR) {
  [System.IO.Path]::GetFullPath($env:COREVIDEO_ZOOM_RUNTIME_DIR)
} else {
  Join-Path $repoRoot "native-core\zoom-runtime\windows\x64"
}

Test-PathEvidence `
  -Name "Dev native core artifact" `
  -Path $devNative `
  -PassNotes "Found native/build-dev/corevideo-native.exe." `
  -FailNotes "Missing native/build-dev/corevideo-native.exe. Run npm run build:native-dev after staging the Zoom SDK."

Test-PathEvidence `
  -Name "Zoom helper artifact" `
  -Path $devZoomEngine `
  -PassNotes "Found native/build-dev/corevideo-zoom-engine.exe." `
  -FailNotes "Missing native/build-dev/corevideo-zoom-engine.exe. Run npm run build:native-dev with Zoom engine enabled."

Test-PathEvidence `
  -Name "Staged Zoom runtime" `
  -Path (Join-Path $zoomRuntime "bin\sdk.dll") `
  -PassNotes "Found staged Zoom runtime at $zoomRuntime." `
  -FailNotes "Missing staged Zoom runtime. Run scripts/stage-zoom-sdk.ps1 or set COREVIDEO_ZOOM_RUNTIME_DIR."

if (-not $SkipDevReadiness) {
  Invoke-AlphaStep `
    -Name "Native shell dev readiness" `
    -Command "npm run test:native-shell-dev-readiness -- -NoPublish" `
    -AllowFailure
}
else {
  Add-Result -Name "Native shell dev readiness" -Status "skipped" -ExitCode 0 -LogPath "" -Notes "Skipped by parameter."
}

if (-not $SkipRecordStream) {
  Invoke-AlphaStep `
    -Name "Record stream harness" `
    -Command "npm run validate:record-stream -- --destinations recording --timeout-ms 30000" `
    -AllowFailure
}
else {
  Add-Result -Name "Record stream harness" -Status "skipped" -ExitCode 0 -LogPath "" -Notes "Skipped by parameter."
}

$failed = @($results | Where-Object { $_.status -eq "failed" })
$warnings = @($results | Where-Object { $_.status -eq "warning" })
$passed = @($results | Where-Object { $_.status -eq "passed" })
$skipped = @($results | Where-Object { $_.status -eq "skipped" })
$overall = if ($failed.Count -gt 0) { "failed" } elseif ($warnings.Count -gt 0) { "warning" } else { "passed" }

$json = [pscustomobject]@{
  generatedAt = (Get-Date).ToString("o")
  branch = $branch
  commit = $head
  overall = $overall
  counts = [pscustomobject]@{
    passed = $passed.Count
    warning = $warnings.Count
    failed = $failed.Count
    skipped = $skipped.Count
  }
  results = $results
}
$json | ConvertTo-Json -Depth 6 | Out-File -FilePath $jsonPath -Encoding utf8

$lines = New-Object System.Collections.Generic.List[string]
$generatedLabel = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"
$lines.Add("# CoreVideo Pro Alpha Preflight")
$lines.Add("")
$lines.Add("- Generated: $generatedLabel")
$lines.Add("- Branch: $branch")
$lines.Add("- Commit: $head")
$lines.Add("- Overall: **$overall**")
$lines.Add("- Report artifacts: $artifactRoot")
$lines.Add("")
$lines.Add("## Automated Evidence")
$lines.Add("")
$lines.Add("| Check | Status | Notes | Log |")
$lines.Add("|---|---:|---|---|")
foreach ($result in $results) {
  $log = if ([string]::IsNullOrWhiteSpace($result.logPath)) { "" } else { $result.logPath }
  $notes = ($result.notes -replace "\|", "\|")
  $lines.Add("| $($result.name) | $($result.status) | $notes | $log |")
}
$lines.Add("")
$lines.Add("## Manual Home Alpha Pass")
$lines.Add("")
$lines.Add("Run these when you can interact with the app and a real Zoom meeting:")
$lines.Add("")
$lines.Add("1. Launch: npm run run:studio or npm run launch:native.")
$lines.Add("2. Confirm Program/Preview show first-frame/live/degraded/error status instead of stale compositor waiting text.")
$lines.Add("3. Settings: select a recent meeting if available, join Zoom, then turn capture on.")
$lines.Add("4. Sources: assign a Zoom participant and a UVC webcam; confirm source dropdowns update.")
$lines.Add("5. Routing: route ISO A and ISO 1, switch to another source, then click again/remove to prove one-source isolation.")
$lines.Add("6. Media: import an asset, select it, play/pause it, and assign it as Brand Kit logo from Overlays.")
$lines.Add("7. Record: start a short MP4 recording and confirm output health plus file bytes on disk.")
$lines.Add("8. Leave/rejoin Zoom and confirm recent meeting memory, roster reset, and warnings remain readable.")
$lines.Add("")
$lines.Add("## Decision")
$lines.Add("")
if ($overall -eq "passed") {
  $lines.Add("Offline alpha preflight is green. The remaining risk is live Zoom/device validation.")
}
elseif ($overall -eq "warning") {
  $lines.Add("Offline core gates passed, but one or more dev-machine/live-adapter checks need attention before a confident alpha pass.")
}
else {
  $lines.Add("Offline alpha preflight failed. Fix failed checks before spending manual time on live Zoom validation.")
}

$lines | Out-File -FilePath $reportPath -Encoding utf8

Write-Host ""
Write-Host "[alpha-preflight] overall: $overall" -ForegroundColor $(if ($overall -eq "failed") { "Red" } elseif ($overall -eq "warning") { "Yellow" } else { "Green" })
Write-Host "[alpha-preflight] report: $reportPath"
Write-Host "[alpha-preflight] json:   $jsonPath"

if ($failed.Count -gt 0) {
  exit 1
}

exit 0
