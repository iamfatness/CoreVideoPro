$ErrorActionPreference = 'Stop'
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
$hash = (Get-FileHash -LiteralPath $ownerDll).Hash.ToLowerInvariant()
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
Write-Output "$count ownership/path cases passed; installer script parsed. Only process-owned temporary files changed; production registry/runtime untouched."
