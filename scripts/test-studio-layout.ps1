param([string]$Executable, [string]$ReportDirectory)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Executable) { $Executable = Join-Path $repoRoot 'native-shell/CoreVideoPro.WinUI/bin/x64/Release/net9.0-windows10.0.19041.0/win-x64/CoreVideoPro.WinUI.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $repoRoot 'artifacts/test-results/studio-layout' }
$ReportDirectory = [IO.Path]::GetFullPath($ReportDirectory)
if (Test-Path -LiteralPath $ReportDirectory) { throw 'Use a new layout report directory; old successes must not mask a failed probe.' }
New-Item -ItemType Directory -Path $ReportDirectory | Out-Null
foreach ($kind in @('studio','graphics','operator-settings')) {
    $flag = switch ($kind) { 'studio' { '--verify-studio-layout' } 'graphics' { '--verify-graphics-workspace' } default { '--verify-operator-settings' } }
    $report = Join-Path $ReportDirectory ($kind + '.json')
    $probe = Start-Process -FilePath $Executable -ArgumentList @($flag, ('"' + $report + '"')) -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not $probe.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -gt $deadline) { $probe.Kill(); throw "$kind layout probe timed out." }
    }
    $probe.Refresh()
    if (-not (Test-Path -LiteralPath $report)) { throw "$kind layout probe exited $($probe.ExitCode) without a report." }
    $result = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    if ($probe.ExitCode -ne 0 -or -not $result.passed -or $result.checks.Count -eq 0) { throw "$kind layout failed: $($result.error)" }
    Write-Output "$kind layout passed: $($result.checks.Count) checks."
}
