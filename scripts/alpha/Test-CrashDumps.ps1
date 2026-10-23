# #433: exercises Install-CrashDumps.ps1 against a throwaway HKCU key, so it runs
# unelevated on any machine and never touches the real HKLM LocalDumps config.
# Includes a run from the 32-bit SysWOW64 PowerShell (what the 32-bit NSIS installer
# launches) to prove the 64-bit registry view is used despite WOW64 redirection.
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'Install-CrashDumps.ps1'
$ps = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$ps32 = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
$id = [Guid]::NewGuid().ToString('N')
$base = "Software\CoreVideoProInstallerTests\$id"
$dumps = "$base\LocalDumps"
$uninstall = "$base\Uninstall"
$work = Join-Path ([IO.Path]::GetTempPath()) "cvp-crashdumps-$id"
$checks = 0
function Assert([bool]$c, [string]$m) { if (!$c) { throw $m }; $script:checks++ }
function Run([string[]]$extra, [string]$host_ = $ps) {
    $out = & $host_ -NoProfile -ExecutionPolicy Bypass -File $script -Hive CurrentUser -LocalDumpsPath $dumps -UninstallPath $uninstall @extra
    return @{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}
# The test's own reads are 64-bit-view explicit, independent of the script under test.
function Open64 { [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64) }
function KeyNames { $b = Open64; try { $k = $b.OpenSubKey($dumps); if (-not $k) { return @() }; try { return @($k.GetSubKeyNames()) } finally { $k.Dispose() } } finally { $b.Dispose() } }
function OpenExe([string]$exe) { $b = Open64; try { return $b.OpenSubKey("$dumps\$exe") } finally { $b.Dispose() } }
function AssertValues([string]$exe, [string]$label) {
    $k = OpenExe $exe
    Assert ($null -ne $k) "$label $exe key missing from the 64-bit view"
    try {
        Assert ($k.GetValue('DumpType') -eq 2) "$label $exe DumpType"
        Assert ($k.GetValue('DumpCount') -eq 5) "$label $exe DumpCount"
        Assert ($k.GetValue('DumpFolder', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -eq '%LOCALAPPDATA%\CoreVideoPro\crashes') "$label $exe DumpFolder not the unexpanded path"
        Assert ($k.GetValueKind('DumpFolder') -eq 'ExpandString') "$label $exe DumpFolder is not REG_EXPAND_SZ"
        Assert ($k.GetValueKind('DumpType') -eq 'DWord') "$label $exe DumpType is not DWORD"
        Assert ($null -ne $k.GetValue('CoreVideoProInstaller')) "$label $exe marker missing"
    } finally { $k.Dispose() }
}
$exes = 'CoreVideoPro.WinUI.exe', 'corevideo-native.exe', 'corevideo-zoom-engine.exe'
New-Item -ItemType Directory -Force $work | Out-Null
try {
    # Install writes the ruled values for exactly the three executables.
    $r = Run @('-Action', 'Install', '-ReleaseId', 'beta-2026-01-01-aaaaaaa', '-LogPath', (Join-Path $work 'log.txt'))
    Assert ($r.Code -eq 0) "Install exited $($r.Code): $($r.Out)"
    Assert ((KeyNames).Count -eq 3) 'Expected exactly three LocalDumps keys.'
    foreach ($exe in $exes) { AssertValues $exe 'install' }
    Assert ((Get-Content (Join-Path $work 'log.txt') -Raw) -match 'Enabled full crash dumps') 'Log file not written.'
    # Idempotent: exit code AND values.
    Assert ((Run @('-Action', 'Install')).Code -eq 0) 'Second install failed.'
    Assert ((KeyNames).Count -eq 3) 'Second install changed the key set.'
    foreach ($exe in $exes) { AssertValues $exe 'second install' }
    # Another registered version keeps the keys on uninstall; a gone one does not.
    $b = Open64
    try {
        $o = $b.CreateSubKey("$uninstall\CoreVideoPro-other"); $o.SetValue('InstallLocation', $work); $o.Dispose()
    } finally { $b.Dispose() }
    $r = Run @('-Action', 'Uninstall', '-ReleaseId', 'beta-2026-01-01-aaaaaaa')
    Assert ($r.Code -eq 0 -and (KeyNames).Count -eq 3) 'Uninstall removed keys another install still needs.'
    $b = Open64
    try { $o = $b.OpenSubKey("$uninstall\CoreVideoPro-other", $true); $o.SetValue('InstallLocation', (Join-Path $work 'gone')); $o.Dispose() } finally { $b.Dispose() }
    $r = Run @('-Action', 'Uninstall', '-ReleaseId', 'beta-2026-01-01-aaaaaaa')
    Assert ($r.Code -eq 0 -and (KeyNames).Count -eq 0) "Uninstall left keys: $($r.Out)"
    # A configuration we did not write is preserved on install and on uninstall.
    $b = Open64
    try { $f = $b.CreateSubKey("$dumps\corevideo-native.exe"); $f.SetValue('DumpType', 1, [Microsoft.Win32.RegistryValueKind]::DWord); $f.Dispose() } finally { $b.Dispose() }
    $r = Run @('-Action', 'Install')
    Assert ($r.Code -eq 0 -and $r.Out -match 'PRESERVED') 'Foreign key not reported as preserved.'
    $f = OpenExe 'corevideo-native.exe'
    try { Assert ($f.GetValue('DumpType') -eq 1) 'Foreign key was clobbered.' } finally { $f.Dispose() }
    Run @('-Action', 'Uninstall') | Out-Null
    Assert ((KeyNames) -contains 'corevideo-native.exe') 'Uninstall removed a key this installer did not write.'
    Assert (-not ((KeyNames) -contains 'CoreVideoPro.WinUI.exe')) 'Uninstall left an owned key.'
    Remove-Item -LiteralPath "HKCU:\$dumps" -Recurse -Force
    # WOW64: the 32-bit host (what the 32-bit NSIS installer launches) must still use the 64-bit view.
    if (Test-Path -LiteralPath $ps32) {
        # Software\Classes\CLSID IS redirected for 32-bit processes (a default-view write from SysWOW64
        # PowerShell lands under Classes\WOW6432Node\CLSID). A shared path like Software\CoreVideoProInstallerTests
        # is NOT redirected and would pass even with the old HKLM:-drive code, so use CLSID here.
        $clsid = '{' + [Guid]::NewGuid().ToString() + '}'
        $clsidRoot = "Software\Classes\CLSID\$clsid"
        $dumps = "$clsidRoot\LocalDumps"
        $r = Run @('-Action', 'Install', '-ReleaseId', 'beta-2026-01-01-aaaaaaa') $ps32
        Assert ($r.Code -eq 0) "32-bit host install exited $($r.Code): $($r.Out)"
        Assert ((KeyNames).Count -eq 3) '32-bit host install not visible from the 64-bit view (WOW64 redirection).'
        foreach ($exe in $exes) { AssertValues $exe '32-bit host' }
        # And it must see 64-bit keys it did not write (preserve logic), and remove its own.
        $r = Run @('-Action', 'Uninstall', '-ReleaseId', 'beta-2026-01-01-aaaaaaa') $ps32
        Assert ($r.Code -eq 0 -and (KeyNames).Count -eq 0) '32-bit host uninstall left keys in the 64-bit view.'
        $b = Open64
        try { $f = $b.CreateSubKey("$dumps\corevideo-native.exe"); $f.SetValue('DumpType', 1, [Microsoft.Win32.RegistryValueKind]::DWord); $f.Dispose() } finally { $b.Dispose() }
        $r = Run @('-Action', 'Install') $ps32
        Assert ($r.Out -match 'PRESERVED') '32-bit host did not see the 64-bit foreign key.'
        $hosted32 = $true
        $dumps = "$base\LocalDumps"
    } else { Write-Output 'SKIP: SysWOW64 PowerShell not present; WOW64 case not exercised.'; $hosted32 = $false }
    # A refused write is loud and non-fatal for the caller: exit 2, message, no throw.
    # A key name over the 255-character registry limit makes the write throw.
    $tooLong = 'x' * 300
    $r = & $ps -NoProfile -ExecutionPolicy Bypass -File $script -Action Install -Hive CurrentUser -LocalDumpsPath "$base\$tooLong"
    Assert ($LASTEXITCODE -eq 2 -and ($r -join ' ') -match 'NOT configured') "Failed write was not reported loudly (exit $LASTEXITCODE)."
    # Non-admin HKLM without -AllowElevation must not prompt, and must say so.
    $isAdmin = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        $r = & $ps -NoProfile -ExecutionPolicy Bypass -File $script -Action Install
        Assert ($LASTEXITCODE -eq 2 -and ($r -join ' ') -match 'administrator approval') 'Unelevated HKLM install did not refuse loudly.'
    }
    Write-Output "Test-CrashDumps passed ($checks checks; wow64 host exercised: $hosted32)."
} finally {
    Remove-Item -LiteralPath "HKCU:\$base" -Recurse -Force -ErrorAction SilentlyContinue
    # The shared parent goes too, but only when nothing else (another run) is using it.
    $parent = 'HKCU:\Software\CoreVideoProInstallerTests'
    if ((Test-Path -LiteralPath $parent) -and -not (Get-ChildItem -LiteralPath $parent -ErrorAction SilentlyContinue) -and
        -not (Get-Item -LiteralPath $parent).ValueCount) { Remove-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue }
    # CLSID test key, in the 64-bit view (the 32-bit host's redirected copy, if any, is under WOW6432Node).
    if ($clsidRoot) {
        $b64 = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
        try {
            $b64.DeleteSubKeyTree($clsidRoot, $false)
            $b64.DeleteSubKeyTree("Software\Classes\WOW6432Node\CLSID\$clsid", $false)
        } catch { } finally { $b64.Dispose() }
    }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
