[CmdletBinding()]
param([ValidateSet('Install','Uninstall','Rollback','Inspect')][string]$Action = 'Install',
    [Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$AllowElevation,
    [switch]$MachineOnly,
    [string]$OwnerSid = ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value))
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$app = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $AppDirectory).Path).TrimEnd('\')
$serverKey = 'SOFTWARE\Classes\CLSID\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\InprocServer32'
$machine = Get-CameraMachineRegistration
$runtimeCleanup = 'not_requested'
if ($Action -eq 'Install' -and -not $MachineOnly) {
    $currentUser = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $existingUser = $currentUser.OpenSubKey($serverKey)
        try {
            if ($existingUser) {
                $userPath = [string]$existingUser.GetValue('')
                $userRole = [string]$existingUser.GetValue('CoreVideoOwnerRole')
                if ($userPath -and $userRole -ne 'CoreVideoPro.VirtualCamera.v1' -and -not (Test-OwnedLegacyCamera $userPath)) {
                    throw 'A per-user camera registration with unknown ownership exists; inspect it before migration.'
                }
            }
        } finally { if ($existingUser) { $existingUser.Dispose() } }
    } finally { $currentUser.Dispose() }
}
if ($Action -eq 'Inspect') {
    $actualHash = if ($machine.Path -and (Test-Path -LiteralPath $machine.Path -PathType Leaf)) { (Get-FileHash -LiteralPath $machine.Path).Hash.ToLowerInvariant() } else { '' }
    $loaded = @()
    $moduleInspection = 'unverified'
    try {
        foreach ($service in Get-CimInstance Win32_Service -Filter "Name='FrameServer' OR Name='FrameServerMonitor'") {
            if ($service.ProcessId) {
                $loaded += @(Get-Process -Id $service.ProcessId -Module -ErrorAction Stop |
                    Where-Object ModuleName -eq 'corevideo-virtualcam.dll' | Select-Object FileName,ModuleName)
            }
        }
        $moduleInspection = 'observed'
    } catch { $moduleInspection = 'unverified' }
    [ordered]@{ schema='camera-installation-v1'; registration=$machine; registeredFileSha256=$actualHash;
        registeredFileMatchesManifest=($actualHash -and $actualHash -eq $machine.Sha256); moduleInspection=$moduleInspection;
        loadedModules=$loaded; loadedIdentityMatchesRegistration=($moduleInspection -eq 'observed' -and $loaded.Count -gt 0 -and @($loaded | Where-Object { $_.FileName -ne $machine.Path }).Count -eq 0);
        servingIdentityVerified=$false } | ConvertTo-Json -Depth 5
    exit 0
}
$userBackupPath = Join-Path $app '.camera-registration-before.json'
if ($Action -eq 'Install' -and -not $MachineOnly) {
    $priorUser = Get-CameraMachineRegistration -CurrentUser
    if ($priorUser.AppDirectory -ne $app) {
        $priorUser | ConvertTo-Json | Set-Content -LiteralPath $userBackupPath
    }
}
$ownershipAction = if ($Action -eq 'Rollback') { 'Uninstall' } else { $Action }
$initialDecision = Get-CameraOwnershipDecision -Action $ownershipAction -RegisteredPath $machine.Path -OwnerRole $machine.Role -OwnerAppDirectory $machine.AppDirectory -AppDirectory $app -OwnedLegacy (Test-OwnedLegacyCamera $machine.Path $OwnerSid)
$machineChangeNeeded = $Action -eq 'Install' -or $initialDecision -eq 'RemoveOwned'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
$admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin -and $machineChangeNeeded) {
    if (-not $AllowElevation) { throw 'Camera setup requires administrator approval. Run Register-VirtualCamera.cmd; silent setup must already be elevated.' }
    if ($OwnerSid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value) { throw 'Only the installing user can request elevation.' }
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Action ' + $Action + ' -AppDirectory "' + $app + '" -OwnerSid ' + $OwnerSid + ' -MachineOnly'
    $child = Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -PassThru -Wait
    if ($child.ExitCode -ne 0) { throw "Machine camera $Action failed (exit $($child.ExitCode)); the app's user registration was not changed." }
    if ($Action -eq 'Uninstall' -and $initialDecision -eq 'RemoveOwned') {
        $runtimeCleanup = if (Test-Path -LiteralPath $machine.Path -PathType Leaf) { 'retained_in_use_or_reference' } else { 'removed' }
    }
    $machine = Get-CameraMachineRegistration
} elseif ($admin -and $machineChangeNeeded) {
    $mutexSecurity = [Security.AccessControl.MutexSecurity]::new()
    $mutexSecurity.SetAccessRuleProtection($true, $false)
    $mutexSecurity.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    foreach ($sid in @('S-1-5-18','S-1-5-32-544')) {
        $mutexSecurity.AddAccessRule([Security.AccessControl.MutexAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid), [Security.AccessControl.MutexRights]::FullControl, [Security.AccessControl.AccessControlType]::Allow))
    }
    $created = $false
    $mutex = [Threading.Mutex]::new($false, 'Global\CoreVideoPro.CameraInstallation.v1', [ref]$created, $mutexSecurity)
    if ($mutex.GetAccessControl().GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Camera installer mutex has unknown ownership.' }
    $locked = $false
    try {
    try { $locked = $mutex.WaitOne(30000) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'Another camera installation is still running. Retry after it finishes.' }
    $machine = Get-CameraMachineRegistration
    if (Get-Process -Name 'CoreVideoPro.WinUI','corevideo-native' -ErrorAction SilentlyContinue) { throw 'Close CoreVideo Pro before changing camera installation.' }
    $ownershipAction = if ($Action -eq 'Rollback') { 'Uninstall' } else { $Action }
    $decision = Get-CameraOwnershipDecision -Action $ownershipAction -RegisteredPath $machine.Path -OwnerRole $machine.Role -OwnerAppDirectory $machine.AppDirectory -AppDirectory $app -OwnedLegacy (Test-OwnedLegacyCamera $machine.Path $OwnerSid)
    if ($decision -eq 'Conflict') { throw 'A camera registration with unknown ownership exists. Inspect it before migrating; setup will not overwrite it.' }
    $root = Get-CameraRuntimeRoot
    $backupPath = Join-Path (Join-Path $root 'transactions') ((Get-CameraTransactionName $app) + '.json')
    Assert-CameraRuntimePath $root $backupPath
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        if ($Action -eq 'Install') {
            $source = Join-Path $app 'corevideo-virtualcam.dll'
            $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
            $destination = Join-Path (Join-Path $root $hash) 'corevideo-virtualcam.dll'
            Assert-CameraRuntimePath $root $destination
            New-Item -ItemType Directory -Path $root -Force | Out-Null
            Set-Acl -LiteralPath $root -AclObject (New-CameraRuntimeAcl $true)
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Set-Acl -LiteralPath (Split-Path -Parent $destination) -AclObject (New-CameraRuntimeAcl $true)
            if (Test-Path -LiteralPath $destination) {
                if ((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $hash) { throw 'Protected camera DLL hash does not match its version directory.' }
            } else { Copy-Item -LiteralPath $source -Destination $destination }
            if ((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $hash) { throw 'Camera DLL changed during installation.' }
            # Only our own machine runtime directories/DLL receive these ACLs.
            # Administrators own them; users and LocalService can read/execute.
            Set-Acl -LiteralPath $destination -AclObject (New-CameraRuntimeAcl $false)
            New-Item -ItemType Directory -Path (Split-Path -Parent $backupPath) -Force | Out-Null
            Set-Acl -LiteralPath (Split-Path -Parent $backupPath) -AclObject (New-CameraRuntimeAcl $true)
            if ($machine.AppDirectory -ne $app -or $machine.Path -ne $destination) {
                $previousHash = if ($machine.Path -and (Test-Path -LiteralPath $machine.Path -PathType Leaf)) { (Get-FileHash -LiteralPath $machine.Path).Hash } else { '' }
                $backup = [ordered]@{ previous=$machine; previousHash=$previousHash; installed=$destination; owner=$app }
                $backup | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $backupPath
                Set-Acl -LiteralPath $backupPath -AclObject (New-CameraRuntimeAcl $false)
            }
            try {
                Set-CameraMachineRegistration ([ordered]@{Path=$destination;ThreadingModel='Both';Role='CoreVideoPro.VirtualCamera.v1';AppDirectory=$app;Sha256=$hash})
                $installed = Get-CameraMachineRegistration
                if ($installed.Path -ne $destination -or $installed.Sha256 -ne $hash -or $installed.AppDirectory -ne $app) { throw 'Machine registration verification failed.' }
            } catch { Set-CameraMachineRegistration $machine; throw }
            $machine = $installed
        } elseif ($decision -eq 'RemoveOwned' -and $Action -eq 'Rollback') {
            Assert-CameraRuntimePath $root $machine.Path
            $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
            if ($backup.owner -ne $app -or $backup.installed -ne $machine.Path) { throw 'Rollback ownership mismatch.' }
            if ($backup.previous.Path) {
                if (-not (Test-Path -LiteralPath $backup.previous.Path -PathType Leaf)) { throw 'Previous camera DLL is missing; rollback cannot be completed.' }
                if ((Get-FileHash -LiteralPath $backup.previous.Path).Hash -ne $backup.previousHash) { throw 'Previous camera DLL changed; rollback cannot be completed.' }
                if ($backup.previous.Role -eq 'CoreVideoPro.VirtualCamera.v1' -and -not (Test-Path -LiteralPath (Join-Path $backup.previous.AppDirectory 'corevideo-virtualcam.dll') -PathType Leaf)) { throw 'Previous owning installation was removed; rollback cannot restore that owner.' }
            }
            Set-CameraMachineRegistration $backup.previous
            $machine = Get-CameraMachineRegistration
        } elseif ($decision -eq 'RemoveOwned') {
            Assert-CameraRuntimePath $root $machine.Path
            $removedPath = $machine.Path
            $registry.DeleteSubKeyTree($serverKey, $false)
            $machine = Get-CameraMachineRegistration
            $runtimeCleanup = Remove-UnreferencedCameraRuntime $root $removedPath
        }
    } finally { $registry.Dispose() }
    } finally { if ($locked) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
}
if (-not $MachineOnly) {
    $currentUser = Get-CameraMachineRegistration -CurrentUser
    if ($Action -eq 'Install') {
        if ($machine.AppDirectory -ne $app -or $machine.Role -ne 'CoreVideoPro.VirtualCamera.v1') { throw 'Another installation replaced camera ownership; retry setup.' }
        try { Set-CameraMachineRegistration $machine -CurrentUser }
        catch {
            Set-CameraMachineRegistration $currentUser -CurrentUser
            & $PSCommandPath -Action Rollback -AppDirectory $app -AllowElevation:$AllowElevation -MachineOnly
            throw
        }
    } elseif ($currentUser.Role -eq 'CoreVideoPro.VirtualCamera.v1' -and [string]::Equals($currentUser.AppDirectory, $app, [StringComparison]::OrdinalIgnoreCase)) {
        if ($Action -eq 'Rollback') {
            $savedUser = Get-Content -LiteralPath $userBackupPath -Raw | ConvertFrom-Json
            Set-CameraMachineRegistration $savedUser -CurrentUser
        } else { Set-CameraMachineRegistration ([ordered]@{Path=''}) -CurrentUser }
    }
}
[ordered]@{ schema='camera-installation-v1'; action=$Action; registration=$machine; runtimeCleanup=$runtimeCleanup; servingIdentityVerified=$false;
    note='Registration verified; camera activation and actual loaded-module inspection are required. Close camera consumers or restart Windows if an older DLL remains loaded.' } | ConvertTo-Json -Depth 5
