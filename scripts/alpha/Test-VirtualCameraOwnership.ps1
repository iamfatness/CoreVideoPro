$ErrorActionPreference = 'Stop'
# Reproduce the installer's module-resolution failure for all identity checks.
function Get-FileHash { throw 'Get-FileHash must not be required by camera setup.' }
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$count = 0
function Assert-Decision([string]$Expected, [hashtable]$Arguments) {
    $actual = Get-CameraOwnershipDecision @Arguments
    if ($actual -ne $Expected) { throw "Expected $Expected; got $actual" }
    $script:count++
}
$owned = @{RegisteredPath='C:\Program Files\Common Files\CoreVideoPro\Camera\abc\corevideo-virtualcam.dll';OwnerRole='CoreVideoPro.VirtualCamera.v1';OwnerAppDirectory='C:\Apps\new';AppDirectory='C:\Apps\old'}
Assert-Decision 'PreserveNewerOwner' ($owned + @{Action='Uninstall'})
Assert-Decision 'ReplaceOwned' ($owned + @{Action='Install'})
$owned.AppDirectory = 'c:\apps\NEW\'
Assert-Decision 'RemoveOwned' ($owned + @{Action='Uninstall'})
Assert-Decision 'Conflict' @{Action='Install';RegisteredPath='C:\Other\foreign.dll';AppDirectory='C:\Apps\new'}
Assert-Decision 'PreserveUnknownOwner' @{Action='Uninstall';RegisteredPath='C:\Other\foreign.dll';AppDirectory='C:\Apps\new'}
Assert-Decision 'MigrateOwnedLegacy' @{Action='Install';RegisteredPath='C:\Apps\old\corevideo-virtualcam.dll';AppDirectory='C:\Apps\new';OwnedLegacy=$true}
Assert-Decision 'Absent' @{Action='Install';RegisteredPath='';AppDirectory='C:\Apps\new'}
Assert-Decision 'Absent' @{Action='Uninstall';RegisteredPath='';AppDirectory='C:\Apps\new'}
Assert-Decision 'Conflict' @{Action='Install';RegisteredPath='C:\Owned\camera.dll';OwnerRole='CoreVideoPro.VirtualCamera.v1';AppDirectory='C:\Apps\new'}
Assert-Decision 'PreserveUnknownOwner' @{Action='Uninstall';RegisteredPath='C:\Owned\camera.dll';OwnerRole='CoreVideoPro.VirtualCamera.v1';AppDirectory='C:\Apps\new'}
if ((Get-CameraTransactionName 'C:\Apps\new') -ne (Get-CameraTransactionName 'c:\apps\NEW\')) { throw 'Transaction name changes with equivalent app paths' }
if ((Get-CameraTransactionName 'C:\Apps\new') -eq (Get-CameraTransactionName 'C:\Apps\old')) { throw 'Different installations share a rollback record' }
$count += 2
foreach ($path in @('C:\Outside\corevideo-virtualcam.dll','C:\Protected\Camera\..\outside.dll')) {
    $rejected = $false
    try { Assert-CameraRuntimePath -Root 'C:\Protected\Camera' -Path $path } catch { $rejected = $true }
    if (-not $rejected) { throw 'Escaping runtime target was accepted' }
    $count++
}
$errors = $null; $tokens = $null
foreach ($directory in @($true, $false)) {
    $acl = New-CameraRuntimeAcl $directory
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne 'S-1-5-32-544') { throw 'Camera runtime is not administrator-owned' }
    foreach ($sid in @('S-1-5-19', 'S-1-5-32-545')) {
        $rule = @($acl.GetAccessRules($true, $false, [Security.Principal.SecurityIdentifier]) | Where-Object { $_.IdentityReference.Value -eq $sid })
        if ($rule.Count -ne 1 -or $rule[0].FileSystemRights -ne ([Security.AccessControl.FileSystemRights]::ReadAndExecute -bor [Security.AccessControl.FileSystemRights]::Synchronize)) { throw 'Camera service/users can write the protected runtime, or cannot load it' }
        $count++
    }
}
[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Install-VirtualCamera.ps1'), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { throw ($errors | Out-String) }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('CoreVideoCameraCleanup-' + [Guid]::NewGuid().ToString('N'))
$transactions = Join-Path $temporary 'transactions'
New-Item -ItemType Directory -Path $transactions -Force | Out-Null
$ownerDll = Join-Path $temporary 'corevideo-virtualcam.dll'
$bytes = [byte[]]@(1,2,3,4)
[IO.File]::WriteAllBytes($ownerDll, $bytes)
$hash = Get-CameraPayloadSha256 $ownerDll
if ($hash -ne '9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a') { throw 'Camera payload SHA256 differs from known byte vector.' }
$count++
$missingRejected = $false
try { Get-CameraPayloadSha256 (Join-Path $temporary 'missing.dll') | Out-Null } catch { $missingRejected = $true }
if (-not $missingRejected) { throw 'Missing camera payload received a usable identity.' }
$count++
$version = Join-Path $temporary $hash
New-Item -ItemType Directory -Path $version | Out-Null
$runtime = Join-Path $version 'corevideo-virtualcam.dll'
[IO.File]::WriteAllBytes($runtime, $bytes)
$record = Join-Path $transactions 'test.json'
try {
    @{owner=$temporary;previous=@{Path=$runtime}} | ConvertTo-Json | Set-Content -LiteralPath $record
    if ((Remove-UnreferencedCameraRuntime $temporary $runtime) -ne 'retained_rollback_reference') { throw 'Rollback runtime was deleted' }
    $count++
    [IO.File]::Delete($record)
    $held = [IO.File]::Open($runtime, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        if ((Remove-UnreferencedCameraRuntime $temporary $runtime) -ne 'retained_in_use_or_unverified') { throw 'Locked runtime was deleted' }
        $count++
    } finally { $held.Dispose() }
    [IO.File]::WriteAllBytes($runtime, [byte[]]@(5,6))
    if ((Remove-UnreferencedCameraRuntime $temporary $runtime) -ne 'retained_hash_conflict') { throw 'Changed runtime was deleted' }
    $count++
    [IO.File]::WriteAllBytes($runtime, $bytes)
    if ((Remove-UnreferencedCameraRuntime $temporary $runtime) -ne 'removed') { throw 'Unreferenced/unlocked runtime was not removed' }
    $count++
} finally {
    # Exact test-owned files and empty directories only; no recursive delete.
    foreach ($file in @($record,$runtime,$ownerDll)) { if ([IO.File]::Exists($file)) { [IO.File]::Delete($file) } }
    if ([IO.Directory]::Exists($version)) { [IO.Directory]::Delete($version, $false) }
    [IO.Directory]::Delete($transactions, $false); [IO.Directory]::Delete($temporary, $false)
}
# #781: Frame Server refresh decision. Current/NotLoaded/NotRunning must never
# restart the service; Stale/Unknown must. Module evidence wins over the serve log.
$expected = 'C:\Program Files\Common Files\CoreVideoProCamera\' + ('b' * 64) + '\corevideo-virtualcam.dll'
$old = 'C:\Program Files\Common Files\CoreVideoProCamera\' + ('a' * 64) + '\corevideo-virtualcam.dll'
function Assert-Refresh([string]$Expected, [hashtable]$Arguments) {
    $actual = Get-FrameServerRefreshDecision @Arguments
    if ($actual -ne $Expected) { throw "Refresh decision: expected $Expected; got $actual for $($Arguments | ConvertTo-Json -Compress)" }
    $script:count++
}
Assert-Refresh 'NotRunning' @{ServiceState='Stopped';ExpectedDll=$expected;ModuleInspection='unavailable'}
Assert-Refresh 'NotRunning' @{ServiceState='Stopped';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@($old)}
Assert-Refresh 'NotRunning' @{ServiceState='Absent';ExpectedDll=$expected;ModuleInspection='unavailable'}
Assert-Refresh 'Unknown' @{ServiceState='StopPending';ExpectedDll=$expected;ModuleInspection='unavailable'}
Assert-Refresh 'Current' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@('C:\Windows\System32\mfsensorgroup.dll',$expected)}
Assert-Refresh 'Current' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@($expected.ToUpperInvariant())}
Assert-Refresh 'Current' @{ServiceState='Running';ExpectedDll=($expected + '\');ModuleInspection='observed';LoadedPaths=@($expected.Replace('\','/'))}
Assert-Refresh 'Current' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@('\\?\' + $expected)}
Assert-Refresh 'NotLoaded' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@('C:\Windows\System32\mfsensorgroup.dll')}
Assert-Refresh 'NotLoaded' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@()}
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@($old)}
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@($expected,$old)}
# Observed modules outrank an older serve-log claim in either direction.
Assert-Refresh 'NotLoaded' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@();ServeLogPaths=@($old)}
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='observed';LoadedPaths=@($old);ServeLogPaths=@($expected)}
# Module API unavailable: the serve log for this PID decides, last line wins.
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='unavailable';ServeLogPaths=@($expected,$old)}
Assert-Refresh 'Current' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='unavailable';ServeLogPaths=@($old,$expected)}
Assert-Refresh 'Unknown' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='unavailable';ServeLogPaths=@()}
Assert-Refresh 'Unknown' @{ServiceState='Running';ExpectedDll=$expected;ModuleInspection='unavailable';ServeLogPaths=@('C:\Other\not-the-camera.dll')}
# Uninstall/rollback-to-nothing: nothing is registered, so any loaded CoreVideo camera is stale.
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll='';ModuleInspection='observed';LoadedPaths=@($expected)}
Assert-Refresh 'NotLoaded' @{ServiceState='Running';ExpectedDll='';ModuleInspection='observed';LoadedPaths=@()}
Assert-Refresh 'Stale' @{ServiceState='Running';ExpectedDll='';ModuleInspection='unavailable';ServeLogPaths=@($expected)}
# Serve-log parsing: exact dllmain.cpp shape, PID filter, order kept, hostile lines ignored.
$lines = @(
    '[01:02:03.004 pid=4242] ClassFactory::CreateInstance (DLL loaded by a consumer)',
    "[01:02:03.005 pid=4242] Serving module path=$old",
    "[01:02:04.005 pid=999] Serving module path=$expected",
    "[01:02:05.006 pid=4242] Serving module path=$expected",
    ("[01:02:06.007 pid=4242] Serving module path=C:\" + ('x' * 5000) + '\corevideo-virtualcam.dll'),
    ("[01:02:07.008 pid=4242] Serving module path=C:\evil" + [char]7 + "\corevideo-virtualcam.dll"),
    "[01:02:08.009 pid=4242] Serving module path=relative\corevideo-virtualcam.dll",
    "garbage [01:02:09.010 pid=4242] Serving module path=$old",
    "[01:02:10.011 pid=42420] Serving module path=$old",
    "[01:02:11.012 pid=99999999999999999999] Serving module path=$old")
$parsed = @(Read-ServeLogModulePaths -Lines $lines -ProcessId 4242)
if ($parsed.Count -ne 2 -or $parsed[0] -ne $old -or $parsed[1] -ne $expected) { throw "Serve-log parse wrong: $($parsed -join ' | ')" }
$count++
if (@(Read-ServeLogModulePaths -Lines @() -ProcessId 4242).Count -ne 0 -or @(Read-ServeLogModulePaths -Lines $lines -ProcessId 0).Count -ne 0) { throw 'Empty log or PID 0 produced serving evidence.' }
$count++
# Native PowerShell: Sysnative only for a 32-bit process on 64-bit Windows.
$nativeCases = @(
    @{Os=$true;Process=$false;Expect='C:\WinTest\Sysnative\WindowsPowerShell\v1.0\powershell.exe'},
    @{Os=$true;Process=$true;Expect='C:\WinTest\System32\WindowsPowerShell\v1.0\powershell.exe'},
    @{Os=$false;Process=$false;Expect='C:\WinTest\System32\WindowsPowerShell\v1.0\powershell.exe'})
foreach ($case in $nativeCases) {
    $got = Get-NativePowerShellPath -Is64BitOperatingSystem $case.Os -Is64BitProcess $case.Process -WindowsDirectory 'C:\WinTest'
    if ($got -ne $case.Expect) { throw "Native PowerShell path: expected $($case.Expect); got $got" }
    $count++
}
$hostNative = Get-NativePowerShellPath
if (-not (Test-Path -LiteralPath $hostNative -PathType Leaf)) { throw "Resolved native PowerShell does not exist from this host: $hostNative" }
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess -and $hostNative -notlike '*\Sysnative\*') { throw 'A 32-bit host did not resolve Sysnative.' }
$count++
Write-Output "$count ownership/path cases passed; installer script parsed. Only process-owned temporary files changed; production registry/runtime untouched."
