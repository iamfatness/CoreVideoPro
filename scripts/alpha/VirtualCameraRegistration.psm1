Set-StrictMode -Version Latest

function Get-CameraPayloadSha256 {
    param([Parameter(Mandatory=$true)][string]$Path)
    # NSIS can inherit a PowerShell module path without Get-FileHash. Keep every
    # install/inspect/rollback identity check independent of module discovery.
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $hash.Dispose()
    }
}

function Get-CameraOwnershipDecision {
    param([ValidateSet('Install','Uninstall')][string]$Action,
        [string]$RegisteredPath, [string]$OwnerRole, [string]$OwnerAppDirectory,
        [string]$AppDirectory, [bool]$OwnedLegacy = $false)
    if ([string]::IsNullOrWhiteSpace($RegisteredPath)) { return 'Absent' }
    if ($OwnerRole -eq 'CoreVideoPro.VirtualCamera.v1') {
        if ([string]::IsNullOrWhiteSpace($OwnerAppDirectory)) {
            if ($Action -eq 'Install') { return 'Conflict' }
            return 'PreserveUnknownOwner'
        }
        if ($Action -eq 'Install') { return 'ReplaceOwned' }
        if ([string]::Equals($OwnerAppDirectory.TrimEnd('\'), $AppDirectory.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { return 'RemoveOwned' }
        return 'PreserveNewerOwner'
    }
    if ($Action -eq 'Install' -and $OwnedLegacy) { return 'MigrateOwnedLegacy' }
    if ($Action -eq 'Uninstall') { return 'PreserveUnknownOwner' }
    return 'Conflict'
}

function Get-CameraMachineRegistration {
    param([switch]$CurrentUser)
    $hive = if ($CurrentUser) { [Microsoft.Win32.RegistryHive]::CurrentUser } else { [Microsoft.Win32.RegistryHive]::LocalMachine }
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $machine.OpenSubKey('SOFTWARE\Classes\CLSID\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\InprocServer32')
        try {
            [ordered]@{ Path=if ($key) { [string]$key.GetValue('') } else { '' }
                Role=if ($key) { [string]$key.GetValue('CoreVideoOwnerRole') } else { '' }
                AppDirectory=if ($key) { [string]$key.GetValue('CoreVideoOwnerAppDirectory') } else { '' }
                Sha256=if ($key) { [string]$key.GetValue('CoreVideoSha256') } else { '' }
                ThreadingModel=if ($key) { [string]$key.GetValue('ThreadingModel') } else { '' } }
        } finally { if ($key) { $key.Dispose() } }
    } finally { $machine.Dispose() }
}

function Test-OwnedLegacyCamera {
    param([string]$Path, [string]$OwnerSid = ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value))
    if (-not $Path -or [IO.Path]::GetFileName($Path) -ne 'corevideo-virtualcam.dll') { return $false }
    $directory = Split-Path -Parent $Path
    $marker = Join-Path $directory '.corevideo-prerelease-install'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) { return $false }
    $release = (Get-Content -LiteralPath $marker -Raw).Trim()
    if ($release -notmatch '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { return $false }
    if ($OwnerSid -notmatch '^S-1-5-21-[0-9]+-[0-9]+-[0-9]+-[0-9]+$') { return $false }
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::Users, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $user.OpenSubKey("$OwnerSid\Software\Microsoft\Windows\CurrentVersion\Uninstall\CoreVideoPro-$release")
        try { return $key -and [string]::Equals([string]$key.GetValue('InstallLocation'), $directory, [StringComparison]::OrdinalIgnoreCase) }
        finally { if ($key) { $key.Dispose() } }
    } finally { $user.Dispose() }
}

function Set-CameraMachineRegistration {
    param($Registration, [switch]$CurrentUser)
    $hive = if ($CurrentUser) { [Microsoft.Win32.RegistryHive]::CurrentUser } else { [Microsoft.Win32.RegistryHive]::LocalMachine }
    $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive, [Microsoft.Win32.RegistryView]::Registry64)
    $path = 'SOFTWARE\Classes\CLSID\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\InprocServer32'
    try {
        if (-not $Registration.Path) { $registry.DeleteSubKeyTree($path, $false); return }
        $key = $registry.CreateSubKey($path)
        try {
            $key.SetValue('', [string]$Registration.Path)
            foreach ($pair in @(@('ThreadingModel','ThreadingModel'), @('CoreVideoOwnerRole','Role'), @('CoreVideoOwnerAppDirectory','AppDirectory'), @('CoreVideoSha256','Sha256'))) {
                $value = [string]$Registration.($pair[1])
                if ($value) { $key.SetValue($pair[0], $value) } else { $key.DeleteValue($pair[0], $false) }
            }
        } finally { $key.Dispose() }
    } finally { $registry.Dispose() }
}

function Get-CameraTransactionName {
    param([string]$AppDirectory)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($AppDirectory.TrimEnd('\').ToLowerInvariant())))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-CameraRuntimeRoot {
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $machine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion')
        try { $common = [string]$key.GetValue('CommonFilesDir') } finally { $key.Dispose() }
    } finally { $machine.Dispose() }
    if (-not [IO.Path]::IsPathRooted($common)) { throw 'Windows CommonFilesDir is unavailable.' }
    Join-Path $common 'CoreVideoProCamera'
}

function New-CameraRuntimeAcl {
    param([bool]$Directory)
    $acl = if ($Directory) { [Security.AccessControl.DirectorySecurity]::new() } else { [Security.AccessControl.FileSecurity]::new() }
    $acl.SetAccessRuleProtection($true, $false)
    $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    $inherit = if ($Directory) { [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit' } else { [Security.AccessControl.InheritanceFlags]::None }
    foreach ($entry in @(@('S-1-5-18','FullControl'), @('S-1-5-32-544','FullControl'), @('S-1-5-19','ReadAndExecute'), @('S-1-5-32-545','ReadAndExecute'))) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
            [Security.Principal.SecurityIdentifier]::new($entry[0]), [Security.AccessControl.FileSystemRights]$entry[1],
            $inherit, [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow))
    }
    return $acl
}

function Assert-CameraRuntimePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Camera target escapes the protected runtime directory.' }
    $current = $full
    while ($current) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Camera runtime paths cannot contain junctions or symbolic links.' }
        $parent = [IO.Path]::GetDirectoryName($current)
        if ($parent -eq $current) { break }
        $current = $parent
    }
}

function Remove-UnreferencedCameraRuntime {
    param([string]$Root, [string]$Path)
    Assert-CameraRuntimePath $Root $Path
    if ((Get-CameraMachineRegistration).Path -eq $Path) { return 'retained_registered' }
    $version = Split-Path -Leaf (Split-Path -Parent $Path)
    if ($version -notmatch '^[a-f0-9]{64}$' -or [IO.Path]::GetFileName($Path) -ne 'corevideo-virtualcam.dll') { return 'retained_unknown_layout' }
    foreach ($record in Get-ChildItem -LiteralPath (Join-Path $Root 'transactions') -Filter '*.json' -File -ErrorAction SilentlyContinue) {
        try {
            $backup = Get-Content -LiteralPath $record.FullName -Raw | ConvertFrom-Json
            if ($backup.previous.Path -eq $Path -and (Test-Path -LiteralPath (Join-Path $backup.owner 'corevideo-virtualcam.dll') -PathType Leaf)) { return 'retained_rollback_reference' }
        } catch { return 'retained_unverified_reference' }
    }
    try {
        # A loaded image cannot be opened for exclusive writing. Do not force
        # removal, schedule a reboot delete, inspect protected processes, or
        # restart camera services underneath another consumer.
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($actual -ne $version) { return 'retained_hash_conflict' }
        [IO.File]::Delete($Path)
        try { [IO.Directory]::Delete((Split-Path -Parent $Path), $false) } catch { }
        return 'removed'
    } catch { return 'retained_in_use_or_unverified' }
}
# ---- #781: Frame Server refresh after a camera DLL upgrade -------------------
# The Frame Server keeps a loaded in-proc camera DLL mapped until its service
# host exits, so a correct registration can still be served by an older image.
# These are the pure halves of Refresh-FrameServer.ps1; they touch nothing.

function ConvertTo-CameraDllPathKey {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    $key = $Path.Trim().Replace('/', '\')
    if ($key.StartsWith('\\?\')) { $key = $key.Substring(4) }
    return $key.TrimEnd('\').ToLowerInvariant()
}

function Test-CoreVideoCameraDllPath {
    param([string]$Path)
    return (ConvertTo-CameraDllPathKey $Path).EndsWith('\corevideo-virtualcam.dll')
}

# NotRunning | Current | NotLoaded | Stale | Unknown. Only Stale and Unknown
# justify stopping the service; Unknown refreshes because a needless restart is
# cheaper than validating against the wrong DLL. Observed modules outrank the
# serve log (a log line is a historical claim, a mapped module is a fact). An
# empty -ExpectedDll means nothing is registered (uninstall/rollback to none),
# so any loaded CoreVideo camera DLL is stale.
function Get-FrameServerRefreshDecision {
    param([string]$ServiceState, [string]$ExpectedDll,
        [ValidateSet('observed','unavailable')][string]$ModuleInspection = 'unavailable',
        [string[]]$LoadedPaths = @(), [string[]]$ServeLogPaths = @())
    if ($ServiceState -in @('Stopped','Absent')) { return 'NotRunning' }
    if ($ServiceState -ne 'Running') { return 'Unknown' }
    $expectedKey = ConvertTo-CameraDllPathKey $ExpectedDll
    if ($ModuleInspection -eq 'observed') {
        $camera = @($LoadedPaths | Where-Object { Test-CoreVideoCameraDllPath $_ })
        if ($camera.Count -eq 0) { return 'NotLoaded' }
        if (-not $expectedKey -or @($camera | Where-Object { (ConvertTo-CameraDllPathKey $_) -ne $expectedKey }).Count) { return 'Stale' }
        return 'Current'
    }
    $served = @($ServeLogPaths | Where-Object { Test-CoreVideoCameraDllPath $_ })
    if ($served.Count -eq 0) { return 'Unknown' }
    if (-not $expectedKey -or (ConvertTo-CameraDllPathKey $served[-1]) -ne $expectedKey) { return 'Stale' }
    return 'Current'
}

# The DLL logs "[hh:mm:ss.mmm pid=N] Serving module path=<GetModuleFileNameW>"
# (native/virtualcam-dll/dllmain.cpp, VcamLog.h). Same line rule as
# scripts/qa/camera_serving_evidence.py. Returns this PID's paths in log order
# (the caller takes the last); overlong, control-character and relative paths
# are dropped rather than trusted.
function Read-ServeLogModulePaths {
    param([string[]]$Lines = @(), [int]$ProcessId)
    if ($ProcessId -le 0) { return @() }
    $result = [Collections.Generic.List[string]]::new()
    foreach ($line in $Lines) {
        if (-not $line -or $line.Length -gt 1100) { continue }
        $match = [regex]::Match($line, '^\[\d{2}:\d{2}:\d{2}\.\d{3} pid=(\d{1,10})\] Serving module path=(.+)$')
        if (-not $match.Success -or [int64]$match.Groups[1].Value -ne $ProcessId) { continue }
        $path = $match.Groups[2].Value.TrimEnd("`r")
        if ($path -match '[\x00-\x1f]' -or $path.Length -gt 1024) { continue }
        try { if (-not [IO.Path]::IsPathRooted($path)) { continue } } catch { continue }
        $result.Add($path)
    }
    return $result.ToArray()
}

# The installer is 32-bit NSIS: its $SYSDIR PowerShell is the SysWOW64 host,
# which cannot enumerate a 64-bit process's modules (measured: not even the
# user's own explorer.exe). Sysnative is the WOW64 alias for the real System32.
# Windows PowerShell 5.1, never $PSHOME: a pwsh 7 parent must not leak in.
function Get-NativePowerShellPath {
    param([bool]$Is64BitOperatingSystem = [Environment]::Is64BitOperatingSystem,
        [bool]$Is64BitProcess = [Environment]::Is64BitProcess,
        [string]$WindowsDirectory = $env:windir)
    $folder = if ($Is64BitOperatingSystem -and -not $Is64BitProcess) { 'Sysnative' } else { 'System32' }
    return Join-Path $WindowsDirectory "$folder\WindowsPowerShell\v1.0\powershell.exe"
}
Export-ModuleMember -Function Get-CameraPayloadSha256,Get-FrameServerRefreshDecision,Read-ServeLogModulePaths,Get-NativePowerShellPath,Test-CoreVideoCameraDllPath,ConvertTo-CameraDllPathKey,Get-CameraOwnershipDecision,Get-CameraMachineRegistration,Set-CameraMachineRegistration,Get-CameraTransactionName,Test-OwnedLegacyCamera,Get-CameraRuntimeRoot,Assert-CameraRuntimePath,New-CameraRuntimeAcl,Remove-UnreferencedCameraRuntime
