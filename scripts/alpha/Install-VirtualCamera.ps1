[CmdletBinding()]
param([ValidateSet('Install','Uninstall','Rollback','Inspect')][string]$Action = 'Install',
    [Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$AllowElevation,
    [switch]$MachineOnly,
    [switch]$Quiet,
    [string]$OwnerSid = ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value),
    # #781 fault injection for installed tests only: replaces the Frame Server with
    # a built-in fake that refuses to stop (Refresh-FrameServer.ps1 -Fault). The
    # real service is never touched in that mode.
    [string]$FrameServerFault = $env:COREVIDEO_FRAMESERVER_FAULT,
    # Inspect only: the DLL's serve log (tests pass a temp file, never the production one).
    [string]$ServeLog = (Join-Path $env:ProgramData 'CoreVideoPro\vcam-serve.log'))
$ErrorActionPreference = 'Stop'
# Preserve a failure outside the destination: NSIS rolls that directory back.
# Registration metadata has no meeting credentials; retain the actual error and
# call stack rather than reporting every camera failure as missing elevation.
trap {
    $cameraFailure = $_
    try {
        $logRoot = Join-Path $env:LOCALAPPDATA 'CoreVideoPro/installer-logs'
        $null = New-Item -ItemType Directory $logRoot -Force
        $identity = [IO.Path]::GetFileName($AppDirectory.TrimEnd('\'))
        if ($identity -notmatch '^[a-zA-Z0-9._-]+$') { $identity = 'camera-setup' }
        [ordered]@{ timeUtc=[DateTime]::UtcNow.ToString('o'); action=$Action; appDirectory=$AppDirectory;
            message=$cameraFailure.Exception.Message; scriptStackTrace=$cameraFailure.ScriptStackTrace } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logRoot ($identity + '-camera-error.json'))
    } catch { }
    throw $cameraFailure
}
# A PowerShell 7 parent can export its module path to NSIS's Windows PowerShell
# child. Load the host's built-in ACL commands explicitly, rather than finding
# an incompatible Microsoft.PowerShell.Security module through that path.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
. (Join-Path $PSScriptRoot 'CameraUserSession.ps1')
$app = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $AppDirectory).Path).TrimEnd('\')
$serverKey = 'SOFTWARE\Classes\CLSID\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\InprocServer32'
$machine = Get-CameraMachineRegistration
$runtimeCleanup = 'not_requested'
# #781: a machine change refreshes any Frame Server instance still serving a
# different CoreVideo camera DLL. The elevated child cannot hand JSON back
# through Start-Process -Verb RunAs, so it leaves the result beside the app.
$frameServerRefresh = $null
$frameServerRefreshExit = 0
$frameServerResultPath = Join-Path $app '.camera-frameserver-refresh.json'
function Invoke-FrameServerRefresh([string]$ExpectedDll) {
    # Always a 64-bit child: the 32-bit installer host cannot see the service
    # host's modules (Refresh-FrameServer.ps1 would relay through Sysnative too).
    $refreshArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'Refresh-FrameServer.ps1'),
        '-Action','Refresh','-ExpectedDll', $(if ($ExpectedDll) { $ExpectedDll } else { '""' }))
    if ($FrameServerFault) { $refreshArgs += @('-Fault', $FrameServerFault) }
    # No 2>&1: in Windows PowerShell 5.1 a redirected native stderr line throws under Stop.
    $output = @(& (Get-NativePowerShellPath) @refreshArgs | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $parsed = $null
    try { $parsed = ($output -join "`n") | ConvertFrom-Json } catch { }
    if (-not $parsed -or $parsed.schema -ne 'frameserver-refresh-v1') {
        $text = ($output -join ' ')
        if ($text.Length -gt 2000) { $text = $text.Substring(0, 2000) }
        $parsed = [pscustomobject][ordered]@{ schema='frameserver-refresh-v1'; result='error'; decision='Unknown'; error="Refresh produced no result (exit $code): $text"; servingIdentityVerified=$false }
        if ($code -eq 0) { $code = 1 }
    }
    # A refresh failure never rolls back a verified registration: only the
    # running instance is stale. Every non-zero refresh is reported as exit 3.
    $script:frameServerRefresh = $parsed
    $script:frameServerRefreshExit = if ($code -eq 0) { 0 } else { 3 }
}
function Get-InstallerLogIdentity {
    $marker = Join-Path $app '.corevideo-prerelease-install'
    if (Test-Path -LiteralPath $marker -PathType Leaf) {
        $release = (Get-Content -LiteralPath $marker -Raw).Trim()
        if ($release -match '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { return $release }
    }
    $leaf = [IO.Path]::GetFileName($app)
    if ($leaf -match '^[a-zA-Z0-9._-]+$') { return $leaf }
    return 'camera-setup'
}
function Write-FrameServerLog {
    if (-not $frameServerRefresh) { return }
    try {
        $logRoot = Join-Path $env:LOCALAPPDATA 'CoreVideoPro\installer-logs'
        $null = New-Item -ItemType Directory $logRoot -Force
        $line = '{0} action={1} exit={2} result={3} decision={4} {5}' -f [DateTime]::UtcNow.ToString('o'), $Action, $frameServerRefreshExit,
            $frameServerRefresh.result, $frameServerRefresh.decision, ($frameServerRefresh | ConvertTo-Json -Depth 5 -Compress)
        # BOM-free: NSIS and support tooling read this file.
        [IO.File]::AppendAllText((Join-Path $logRoot ((Get-InstallerLogIdentity) + '-frameserver.log')), $line + "`r`n", (New-Object Text.UTF8Encoding $false))
    } catch { }
}
if ($Action -eq 'Install' -and -not $MachineOnly) {
    $null = Invoke-CameraUserSession -Action Preflight -AppDirectory $app -OwnerSid $OwnerSid
}
if ($Action -eq 'Inspect') {
    $desktopUser = Invoke-CameraUserSession -Action Inspect -AppDirectory $app -OwnerSid $OwnerSid
    $actualHash = if ($machine.Path -and (Test-Path -LiteralPath $machine.Path -PathType Leaf)) { (Get-CameraPayloadSha256 $machine.Path) } else { '' }
    # #781: the old in-process Get-Process -Module ran in the 32-bit installer
    # host, which cannot enumerate a 64-bit service host, so it reported
    # 'unverified' by construction. Read-only Refresh-FrameServer.ps1 -Action
    # Inspect runs 64-bit, and falls back to this instance's own serve-log lines
    # when module inspection is denied (always, unelevated).
    $fsArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'Refresh-FrameServer.ps1'),
        '-Action','Inspect','-ExpectedDll', $(if ($machine.Path) { $machine.Path } else { '""' }), '-ServeLog', $ServeLog)
    $fsOutput = @(& (Get-NativePowerShellPath) @fsArgs | ForEach-Object { "$_" })
    $fsExit = $LASTEXITCODE
    $frameServer = $null
    try { $frameServer = ($fsOutput -join "`n") | ConvertFrom-Json } catch { }
    if ($fsExit -ne 0 -or -not $frameServer -or $frameServer.schema -ne 'frameserver-refresh-v1') {
        $frameServer = [pscustomobject][ordered]@{ schema='frameserver-refresh-v1'; result='error'; moduleInspection='unavailable'; loadedCameraModules=@(); servedPaths=@()
            servingIdentityVerified=$false; error="Frame Server inspection failed (exit $fsExit)." }
    }
    $moduleInspection = if ($frameServer.moduleInspection -eq 'observed') { 'observed' } else { 'unverified' }
    $moduleInspectionReason = if ($moduleInspection -eq 'observed') { $null } elseif ($frameServer.moduleError) { [string]$frameServer.moduleError } elseif ($frameServer.error) { [string]$frameServer.error } else { "FrameServer $($frameServer.before.state): no service host to inspect" }
    $loaded = @($frameServer.loadedCameraModules | Where-Object { $_ })
    $registeredKey = ConvertTo-CameraDllPathKey $machine.Path
    $matchesManifest = [bool]($actualHash -and $actualHash -eq $machine.Sha256)
    [ordered]@{ schema='camera-installation-v1'; registration=$machine; desktopUser=$desktopUser; registeredFileSha256=$actualHash;
        registeredFileMatchesManifest=$matchesManifest; moduleInspection=$moduleInspection; moduleInspectionReason=$moduleInspectionReason;
        loadedModules=$loaded; loadedIdentityMatchesRegistration=($moduleInspection -eq 'observed' -and $loaded.Count -gt 0 -and @($loaded | Where-Object { (ConvertTo-CameraDllPathKey $_) -ne $registeredKey }).Count -eq 0);
        servedPaths=@($frameServer.servedPaths | Where-Object { $_ }); frameServer=$frameServer
        # True only when the CURRENT FrameServer instance logged "Serving module
        # path=" for the registered DLL and that file matches the manifest hash.
        # Never true before the first activation after a refresh (lazy load).
        servingIdentityVerified=([bool]$frameServer.servingIdentityVerified -and $matchesManifest) } | ConvertTo-Json -Depth 6
    exit 0
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
    if ($FrameServerFault -match '^[a-z]+$') { $arguments += ' -FrameServerFault ' + $FrameServerFault }
    if (Test-Path -LiteralPath $frameServerResultPath) { Remove-Item -LiteralPath $frameServerResultPath -Force }
    # 64-bit child (#781): the Frame Server refresh needs the native host.
    $child = Start-Process (Get-NativePowerShellPath) -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -PassThru -Wait
    if ($child.ExitCode -ne 0 -and $child.ExitCode -ne 3) { throw "Machine camera $Action failed (exit $($child.ExitCode)); the app's user registration was not changed." }
    if (Test-Path -LiteralPath $frameServerResultPath -PathType Leaf) {
        try { $frameServerRefresh = Get-Content -LiteralPath $frameServerResultPath -Raw | ConvertFrom-Json } catch { }
        Remove-Item -LiteralPath $frameServerResultPath -Force -ErrorAction SilentlyContinue
    }
    if ($child.ExitCode -eq 3) {
        $frameServerRefreshExit = 3
        if (-not $frameServerRefresh) { $frameServerRefresh = [pscustomobject][ordered]@{ schema='frameserver-refresh-v1'; result='error'; decision='Unknown'; error='The elevated camera step reported a Frame Server refresh failure without a result file.'; servingIdentityVerified=$false } }
    }
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
            $hash = (Get-CameraPayloadSha256 $source)
            $destination = Join-Path (Join-Path $root $hash) 'corevideo-virtualcam.dll'
            Assert-CameraRuntimePath $root $destination
            New-Item -ItemType Directory -Path $root -Force | Out-Null
            Set-Acl -LiteralPath $root -AclObject (New-CameraRuntimeAcl $true)
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Set-Acl -LiteralPath (Split-Path -Parent $destination) -AclObject (New-CameraRuntimeAcl $true)
            if (Test-Path -LiteralPath $destination) {
                if ((Get-CameraPayloadSha256 $destination) -ne $hash) { throw 'Protected camera DLL hash does not match its version directory.' }
            } else { Copy-Item -LiteralPath $source -Destination $destination }
            if ((Get-CameraPayloadSha256 $destination) -ne $hash) { throw 'Camera DLL changed during installation.' }
            # Only our own machine runtime directories/DLL receive these ACLs.
            # Administrators own them; users and LocalService can read/execute.
            Set-Acl -LiteralPath $destination -AclObject (New-CameraRuntimeAcl $false)
            New-Item -ItemType Directory -Path (Split-Path -Parent $backupPath) -Force | Out-Null
            Set-Acl -LiteralPath (Split-Path -Parent $backupPath) -AclObject (New-CameraRuntimeAcl $true)
            if ($machine.AppDirectory -ne $app -or $machine.Path -ne $destination) {
                $previousHash = if ($machine.Path -and (Test-Path -LiteralPath $machine.Path -PathType Leaf)) { (Get-CameraPayloadSha256 $machine.Path) } else { '' }
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
            Invoke-FrameServerRefresh $destination
        } elseif ($decision -eq 'RemoveOwned' -and $Action -eq 'Rollback') {
            Assert-CameraRuntimePath $root $machine.Path
            $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
            if ($backup.owner -ne $app -or $backup.installed -ne $machine.Path) { throw 'Rollback ownership mismatch.' }
            if ($backup.previous.Path) {
                if (-not (Test-Path -LiteralPath $backup.previous.Path -PathType Leaf)) { throw 'Previous camera DLL is missing; rollback cannot be completed.' }
                if ((Get-CameraPayloadSha256 $backup.previous.Path) -ne $backup.previousHash) { throw 'Previous camera DLL changed; rollback cannot be completed.' }
                if ($backup.previous.Role -eq 'CoreVideoPro.VirtualCamera.v1' -and -not (Test-Path -LiteralPath (Join-Path $backup.previous.AppDirectory 'corevideo-virtualcam.dll') -PathType Leaf)) { throw 'Previous owning installation was removed; rollback cannot restore that owner.' }
            }
            Set-CameraMachineRegistration $backup.previous
            $machine = Get-CameraMachineRegistration
            Invoke-FrameServerRefresh $machine.Path
        } elseif ($decision -eq 'RemoveOwned') {
            Assert-CameraRuntimePath $root $machine.Path
            $removedPath = $machine.Path
            $registry.DeleteSubKeyTree($serverKey, $false)
            $machine = Get-CameraMachineRegistration
            # Before cleanup: a Frame Server still holding the removed DLL keeps
            # it locked (#783 reported retained_in_use_or_reference).
            Invoke-FrameServerRefresh ''
            $runtimeCleanup = Remove-UnreferencedCameraRuntime $root $removedPath
        }
    } finally { $registry.Dispose() }
    } finally { if ($locked) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
    if ($MachineOnly -and $frameServerRefresh) {
        [IO.File]::WriteAllText($frameServerResultPath, ($frameServerRefresh | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
    }
}
if (-not $MachineOnly) {
    try {
        $desktopUser = Invoke-CameraUserSession -Action $Action -AppDirectory $app -OwnerSid $OwnerSid
    } catch {
        $userFailure = $_
        if ($Action -eq 'Install') {
            # A worker may have applied the alias before a reply/transport error.
            # Restore it in the same independent context before machine rollback.
            try { $null = Invoke-CameraUserSession -Action Rollback -AppDirectory $app -OwnerSid $OwnerSid }
            finally { & $PSCommandPath -Action Rollback -AppDirectory $app -AllowElevation:$AllowElevation -MachineOnly }
        }
        throw $userFailure
    }
}
if (-not $MachineOnly) { Write-FrameServerLog }
$refreshWarning = $null
if ($frameServerRefreshExit -eq 3) {
    # Loud even with -Quiet: NSIS shows this line, and a camera trial run now
    # would validate against the wrong DLL.
    $refreshWarning = "WARNING: camera $Action registration verified, but the Windows Frame Server still runs an instance that could not be refreshed ($($frameServerRefresh.result): $($frameServerRefresh.error)). Close camera apps or restart Windows before using or validating the camera."
}
if ($Quiet) {
    if ($refreshWarning) { Write-Output $refreshWarning }
    else {
        $refreshNote = if ($frameServerRefresh) { " Frame Server: $($frameServerRefresh.result) ($($frameServerRefresh.decision))." } else { '' }
        Write-Output "Camera $Action registration verified;$refreshNote camera delivery remains unverified."
    }
} else {
    [ordered]@{ schema='camera-installation-v1'; action=$Action; registration=$machine; desktopUser=$(if (-not $MachineOnly) {$desktopUser} else {$null}); runtimeCleanup=$runtimeCleanup;
        frameServerRefresh=$frameServerRefresh; warning=$refreshWarning; servingIdentityVerified=$false;
        note='Registration verified. Frame Server loads the camera DLL lazily, so serving identity is unverified until a camera activation logs a fresh "Serving module path=" line from the current FrameServer PID (Inspect, or scripts/qa/camera_serving_evidence.py).' } | ConvertTo-Json -Depth 7
}
if ($frameServerRefreshExit -eq 3) { exit 3 }
