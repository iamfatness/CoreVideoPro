param(
    [ValidateRange(5, 86400)][int]$Seconds = 180,
    [string]$Executable,
    [string]$Report
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Executable) {
    $Executable = Join-Path $repoRoot 'native-shell/CoreVideoPro.WinUI/bin/x64/Release/net9.0-windows10.0.19041.0/win-x64/CoreVideoPro.WinUI.exe'
}
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $Report) {
    $Report = Join-Path $repoRoot ('artifacts/test-results/meter-stress-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
}
$Report = [IO.Path]::GetFullPath($Report)
if (Test-Path -LiteralPath $Report) { throw "Use a new report path; an old success must not mask a failed run: $Report" }
New-Item -ItemType Directory -Force (Split-Path $Report -Parent) | Out-Null
$arguments = @('--verify-audio-meters', "$Seconds", ('"' + $Report + '"'))
$probe = Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
Write-Output "Meter stress PID=$($probe.Id), duration=$Seconds seconds, report=$Report"
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 90)
while (-not $probe.WaitForExit(1000)) {
    if ([DateTime]::UtcNow -gt $deadline) {
        # Only terminate this script's own isolated probe, never the operator app.
        $probe.Kill()
        throw "Meter probe timed out (PID $($probe.Id))."
    }
}
if ($probe.ExitCode -ne 0) { throw "Meter probe failed or crashed: exit=$($probe.ExitCode), PID=$($probe.Id)." }
if (-not (Test-Path -LiteralPath $Report)) { throw 'Probe exited without writing its report.' }
$result = Get-Content -LiteralPath $Report -Raw | ConvertFrom-Json
if (-not $result.passed) { throw "Meter probe failed: $($result.error)" }
$result | Format-List
