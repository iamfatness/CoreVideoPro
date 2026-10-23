# #433: exercises Install-CrashDumps.ps1 against a throwaway HKCU key, so it runs
# unelevated on any machine and never touches the real HKLM LocalDumps config.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'Install-CrashDumps.ps1'
$ps = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$id = [Guid]::NewGuid().ToString('N')
$root = "HKCU:\Software\CoreVideoProInstallerTests\$id\LocalDumps"
$uninstall = "HKCU:\Software\CoreVideoProInstallerTests\$id\Uninstall"
$work = Join-Path ([IO.Path]::GetTempPath()) "cvp-crashdumps-$id"
$checks = 0
function Assert([bool]$c, [string]$m) { if (!$c) { throw $m }; $script:checks++ }
function Run([string[]]$extra) {
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $script -RegistryRoot $root -UninstallRoot $uninstall @extra
    return @{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}
$exes = 'CoreVideoPro.WinUI.exe', 'corevideo-native.exe', 'corevideo-zoom-engine.exe'
New-Item -ItemType Directory -Force $work | Out-Null
try {
    # Install writes the ruled values for exactly the three executables.
    $r = Run @('-Action', 'Install', '-ReleaseId', 'beta-2026-01-01-aaaaaaa', '-LogPath', (Join-Path $work 'log.txt'))
    Assert ($r.Code -eq 0) "Install exited $($r.Code): $($r.Out)"
    Assert (@(Get-ChildItem $root).Count -eq 3) 'Expected exactly three LocalDumps keys.'
    foreach ($exe in $exes) {
        $k = Get-Item -LiteralPath (Join-Path $root $exe)
        Assert ($k.GetValue('DumpType') -eq 2) "$exe DumpType"
        Assert ($k.GetValue('DumpCount') -eq 5) "$exe DumpCount"
        Assert ($k.GetValue('DumpFolder', $null, 'DoNotExpandEnvironmentNames') -eq '%LOCALAPPDATA%\CoreVideoPro\crashes') "$exe DumpFolder not the unexpanded path"
        Assert ($k.GetValueKind('DumpFolder') -eq 'ExpandString') "$exe DumpFolder is not REG_EXPAND_SZ"
        Assert ($k.GetValueKind('DumpType') -eq 'DWord') "$exe DumpType is not DWORD"
    }
    Assert ((Get-Content (Join-Path $work 'log.txt') -Raw) -match 'Enabled full crash dumps') 'Log file not written.'
    # Idempotent.
    Assert ((Run @('-Action', 'Install')).Code -eq 0) 'Second install failed.'
    # Another registered version keeps the keys on uninstall; a gone one does not.
    New-Item -Path "$uninstall\CoreVideoPro-other" -Force | Out-Null
    Set-ItemProperty "$uninstall\CoreVideoPro-other" InstallLocation $work
    $r = Run @('-Action', 'Uninstall', '-ReleaseId', 'beta-2026-01-01-aaaaaaa')
    Assert ($r.Code -eq 0 -and @(Get-ChildItem $root).Count -eq 3) 'Uninstall removed keys another install still needs.'
    Set-ItemProperty "$uninstall\CoreVideoPro-other" InstallLocation (Join-Path $work 'gone')
    $r = Run @('-Action', 'Uninstall', '-ReleaseId', 'beta-2026-01-01-aaaaaaa')
    Assert ($r.Code -eq 0 -and @(Get-ChildItem $root).Count -eq 0) "Uninstall left keys: $($r.Out)"
    # A configuration we did not write is preserved on install and on uninstall.
    $foreign = Join-Path $root 'corevideo-native.exe'
    New-Item -Path $foreign -Force | Out-Null
    New-ItemProperty $foreign DumpType -PropertyType DWord -Value 1 | Out-Null
    $r = Run @('-Action', 'Install')
    Assert ($r.Code -eq 0 -and $r.Out -match 'PRESERVED') 'Foreign key not reported as preserved.'
    Assert ((Get-ItemProperty $foreign).DumpType -eq 1) 'Foreign key was clobbered.'
    Run @('-Action', 'Uninstall') | Out-Null
    Assert (Test-Path $foreign) 'Uninstall removed a key this installer did not write.'
    Assert (-not (Test-Path (Join-Path $root 'CoreVideoPro.WinUI.exe'))) 'Uninstall left an owned key.'
    # A refused write is loud and non-fatal for the caller: exit 2, message, no throw.
    $denied = 'NoSuchDrive:\LocalDumps'
    $r = & $ps -NoProfile -ExecutionPolicy Bypass -File $script -Action Install -RegistryRoot $denied
    Assert ($LASTEXITCODE -eq 2 -and ($r -join ' ') -match 'NOT configured') "Failed write was not reported loudly (exit $LASTEXITCODE)."
    # Non-admin HKLM without -AllowElevation must not prompt, and must say so.
    $isAdmin = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        $r = & $ps -NoProfile -ExecutionPolicy Bypass -File $script -Action Install
        Assert ($LASTEXITCODE -eq 2 -and ($r -join ' ') -match 'administrator approval') 'Unelevated HKLM install did not refuse loudly.'
    }
    Write-Output "Test-CrashDumps passed ($checks checks)."
} finally {
    Remove-Item -LiteralPath "HKCU:\Software\CoreVideoProInstallerTests\$id" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
