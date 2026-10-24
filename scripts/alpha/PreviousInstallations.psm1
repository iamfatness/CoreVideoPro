Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# #883: every command the setup cleanup helper (this module + Remove-PreviousInstallations.ps1)
# needs, with the module that owns it. Resolved up front so a broken environment fails
# loudly, by name, before anything is planned or touched.
$script:SetupHelperCommands = [ordered]@{
    'ConvertTo-Json'='Microsoft.PowerShell.Utility'; 'ConvertFrom-Json'='Microsoft.PowerShell.Utility'
    'Select-Object'='Microsoft.PowerShell.Utility'; 'Sort-Object'='Microsoft.PowerShell.Utility'
    'New-Object'='Microsoft.PowerShell.Utility'; 'Get-Date'='Microsoft.PowerShell.Utility'
    'Write-Output'='Microsoft.PowerShell.Utility'; 'Out-Null'='Microsoft.PowerShell.Core'
    'Get-Item'='Microsoft.PowerShell.Management'; 'Get-ItemProperty'='Microsoft.PowerShell.Management'
    'Get-ChildItem'='Microsoft.PowerShell.Management'; 'Get-Content'='Microsoft.PowerShell.Management'
    'Set-Content'='Microsoft.PowerShell.Management'; 'Add-Content'='Microsoft.PowerShell.Management'
    'Remove-Item'='Microsoft.PowerShell.Management'; 'Move-Item'='Microsoft.PowerShell.Management'
    'New-Item'='Microsoft.PowerShell.Management'; 'Test-Path'='Microsoft.PowerShell.Management'
    'Join-Path'='Microsoft.PowerShell.Management'; 'Split-Path'='Microsoft.PowerShell.Management'
    'Resolve-Path'='Microsoft.PowerShell.Management'; 'Get-CimInstance'='CimCmdlets'
    'Where-Object'='Microsoft.PowerShell.Core'; 'ForEach-Object'='Microsoft.PowerShell.Core'
}

function Format-SetupHelperDiagnostics {
    param($Diagnostics)
    return ('psVersion={0}; is64Bit={1}; languageMode={2}; psHome={3}; psModulePathInherited={4}; psModulePathEffective={5}; script={6}' -f
        $Diagnostics.psVersion, $Diagnostics.is64Bit, $Diagnostics.languageMode, $Diagnostics.psHome,
        $Diagnostics.psModulePathInherited, $Diagnostics.psModulePathEffective, $Diagnostics.script)
}

function Initialize-SetupHelperEnvironment {
    # Owner ruling D5 (2026-10-23): the helper may reset PSModulePath for ITS OWN process.
    # A Windows PowerShell 5.1 child that inherits PowerShell 7's PSModulePath binds
    # Get-FileHash/New-Guid to PS7's Core-only Utility module and reports them "not
    # recognized" (reproduced 2026-10-24). Pin the path, import the built-ins by absolute
    # manifest path, then prove every needed command resolves.
    param([string[]]$RequiredCommands = @(), [string]$ScriptPath = '')
    $diagnostics = [ordered]@{
        psVersion = "$($PSVersionTable.PSVersion)"
        is64Bit = [Environment]::Is64BitProcess
        languageMode = "$($ExecutionContext.SessionState.LanguageMode)"
        psHome = $PSHOME
        psModulePathInherited = "$env:PSModulePath"
        psModulePathEffective = ''
        script = $ScriptPath
    }
    $system = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\Modules'
    $env:PSModulePath = (@((Join-Path $PSHOME 'Modules'), $system) | Select-Object -Unique) -join ';'
    $diagnostics.psModulePathEffective = $env:PSModulePath
    foreach ($module in 'Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','CimCmdlets') {
        $manifest = Join-Path $PSHOME "Modules\$module\$module.psd1"
        if (!(Test-Path -LiteralPath $manifest -PathType Leaf)) {
            throw "Setup helper cannot find built-in module $module at $manifest. $(Format-SetupHelperDiagnostics $diagnostics)"
        }
        try { Import-Module -Name $manifest -Global -ErrorAction Stop }
        catch { throw "Setup helper cannot import $module from ${manifest}: $($_.Exception.Message) $(Format-SetupHelperDiagnostics $diagnostics)" }
    }
    $commands = [ordered]@{}
    foreach ($name in $script:SetupHelperCommands.Keys) { $commands[$name] = $script:SetupHelperCommands[$name] }
    foreach ($name in $RequiredCommands) { if ($name -and !$commands.Contains($name)) { $commands[$name] = '(caller)' } }
    foreach ($name in $commands.Keys) {
        if (!(Get-Command -Name $name -ErrorAction SilentlyContinue)) {
            throw "Setup helper cannot resolve $name (module $($commands[$name])). $(Format-SetupHelperDiagnostics $diagnostics)"
        }
    }
    return [pscustomobject]$diagnostics
}

function Get-InstallPayloadSha256 {
    param([string]$Path)
    # Setup's helper must not depend on Get-FileHash command/module resolution.
    # Stream the validated payload rather than allocating an entire DLL in memory.
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '')
    } finally {
        if ($stream) { $stream.Dispose() }
        $hash.Dispose()
    }
}

function Get-OwnedInstallPath {
    param([string]$Root, [string]$Relative)
    if (!$Relative -or [IO.Path]::IsPathRooted($Relative) -or $Relative -match '[:]|(^|[\\/])\.\.([\\/]|$)') {
        throw "Unsafe install manifest path: $Relative"
    }
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $path = [IO.Path]::GetFullPath((Join-Path $rootPath $Relative))
    if (!$path.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes installation.' }
    for ($cursor = $path; $cursor; $cursor = Split-Path -Parent $cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point in install path: $cursor"
        }
    }
    return $path
}

function Get-PreviousInstallationPlan {
    param([string]$CurrentDirectory, [string]$InstallRoot, [string]$RegistryRoot,
          [string[]]$RunningPaths)
    $current = [IO.Path]::GetFullPath($CurrentDirectory).TrimEnd('\')
    $root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
    if ((Split-Path -Parent $current) -ne $root) { throw 'Current installation is outside the expected install root.' }
    $currentId = Split-Path -Leaf $current
    $registration = Get-ItemProperty -LiteralPath (Join-Path $RegistryRoot "CoreVideoPro-$currentId")
    if ($registration.InstallLocation -ne $current -or
        (Get-Content -LiteralPath (Get-OwnedInstallPath $current '.corevideo-prerelease-install') -Raw).Trim() -ne $currentId) {
        throw 'New installation is not committed; previous installations must remain.'
    }
    foreach ($key in Get-ChildItem -LiteralPath $RegistryRoot) {
        if ($key.PSChildName -notmatch '^CoreVideoPro-((alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+)$') { continue }
        $id = $Matches[1]
        if ($id -eq $currentId) { continue }
        $app = Join-Path $root $id
        try {
            $old = Get-ItemProperty -LiteralPath $key.PSPath
            if ($old.InstallLocation -ne $app) { throw 'Registration is outside the owned version directory.' }
            $marker = Get-OwnedInstallPath $app '.corevideo-prerelease-install'
            if ($old.Publisher -ne 'CoreVideo Pro' -or $old.DisplayName -ne "CoreVideo Pro $id") { throw 'Unknown registration owner.' }
            if (!(Test-Path -LiteralPath $app)) {
                [pscustomobject]@{ releaseId=$id; directory=$app; registryKey=$key.PSPath; status='ready'; reason='Stale registration: directory absent.'; files=@(); preserved=@() }
                continue
            }
            if ((Get-Content -LiteralPath $marker -Raw).Trim() -ne $id) { throw 'Install marker mismatch.' }
            if (@($RunningPaths | Where-Object { $_ -and $_.StartsWith($app + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) {
                throw 'Version is still running.'
            }
            $manifestPath = Get-OwnedInstallPath $app 'build-manifest.json'
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            if ($manifest.releaseId -ne $id -or $manifest.sourceCommit -notmatch '^[a-f0-9]{40}$' -or !$manifest.files.Count) {
                throw 'Package manifest identity is invalid.'
            }
            $paths = [Collections.Generic.List[string]]::new()
            $preserved = [Collections.Generic.List[string]]::new()
            foreach ($file in $manifest.files) {
                $path = Get-OwnedInstallPath $app $file.path
                if ($file.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid payload hash.' }
                if (Test-Path -LiteralPath $path -PathType Leaf) {
                    if ((Get-InstallPayloadSha256 -Path $path) -eq $file.sha256) { $paths.Add($path) }
                    else { $preserved.Add($path) }
                }
            }
            $runtimeManifest = Get-OwnedInstallPath $app 'notices/ffmpeg/installed-files.txt'
            if (Test-Path -LiteralPath $runtimeManifest -PathType Leaf) {
                foreach ($relative in Get-Content -LiteralPath $runtimeManifest) {
                    $path = Get-OwnedInstallPath $app $relative
                    # Runtime manifests cannot authorize arbitrary user files.
                    if (($relative -replace '[\\/]+','/') -notmatch '^(ffmpeg\.exe|(?:avcodec|avdevice|avfilter|avformat|avutil|swscale|swresample)-[0-9]+\.dll|notices/ffmpeg/(?:LICENSE[^/]*|COPYING[^/]*|NOTICE[^/]*|README[^/]*|download-provenance\.txt|installed-files\.txt))$') {
                        throw "Unknown media runtime file: $relative"
                    }
                    if (Test-Path -LiteralPath $path -PathType Leaf) { $paths.Add($path) }
                }
                $paths.Add($runtimeManifest)
            }
            [pscustomobject]@{ releaseId=$id; directory=$app; registryKey=$key.PSPath; status='ready'; reason=''; files=@($paths | Select-Object -Unique); preserved=@($preserved) }
        } catch {
            [pscustomobject]@{ releaseId=$id; directory=$app; registryKey=$key.PSPath; status='retained'; reason=$_.Exception.Message; files=@(); preserved=@() }
        }
    }
}

# #883: the ONLY list of installer-owned metadata names. Root-level files with these
# names are retired by both the registered and the residual path. Keeping one list stops
# the drift that stranded 29 folders on .camera-desktop-registration-before.json.
function Get-OwnedInstallMetadataNames {
    return @('Uninstall.exe', '.camera-registration-before.json', '.camera-desktop-registration-before.json',
        '.corevideo-prerelease-install', 'build-manifest.json', 'previous-installations-cleanup.json')
}

$script:ReleaseIdPattern = '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$'
$script:RuntimeFilePattern = '^(ffmpeg\.exe|(?:avcodec|avdevice|avfilter|avformat|avutil|swscale|swresample)-[0-9]+\.dll|notices/ffmpeg/(?:LICENSE[^/]*|COPYING[^/]*|NOTICE[^/]*|README[^/]*|download-provenance\.txt|installed-files\.txt))$'

# Residual version folders: direct children of the install root that look like a release,
# are not the current install and have NO uninstall registration (the registered path,
# Get-PreviousInstallationPlan, cannot see them). Read-only: classifies every file as
# metadata (root-level owned name), payload (hash-proven against the folder's own valid
# manifest, or allowlisted media runtime) or unknown (everything else, incl. Recordings\).
function Get-ResidualInstallationPlan {
    param([string]$CurrentDirectory, [string]$InstallRoot, [string]$RegistryRoot,
          [string[]]$RunningPaths = @(), [string[]]$ProtectedPaths = @())
    $current = [IO.Path]::GetFullPath($CurrentDirectory).TrimEnd('\')
    $root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
    if ((Split-Path -Parent $current) -ne $root) { throw 'Current installation is outside the expected install root.' }
    if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Install root is a reparse point: $root" }
    $registered = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($key in Get-ChildItem -LiteralPath $RegistryRoot) {
        if ($key.PSChildName -match '^CoreVideoPro-(.+)$') { [void]$registered.Add($Matches[1]) }
        $location = (Get-ItemProperty -LiteralPath $key.PSPath).PSObject.Properties['InstallLocation']
        if ($location -and $location.Value) { [void]$registered.Add((Split-Path -Leaf ([string]$location.Value).TrimEnd('\'))) }
    }
    $metadataNames = Get-OwnedInstallMetadataNames
    foreach ($folder in Get-ChildItem -LiteralPath $root -Directory -Force) {
        $id = $folder.Name
        $dir = Join-Path $root $id
        if ($id -notmatch $script:ReleaseIdPattern -or $dir -eq $current -or $registered.Contains($id)) { continue }
        $plan = [pscustomobject]@{ releaseId=$id; directory=$dir; status='ready'; reason=''; metadata=@(); payload=@(); unknown=@() }
        try {
            if ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point: $dir" }
            if (@($RunningPaths | Where-Object { $_ -and $_.StartsWith($dir + '\', [StringComparison]::OrdinalIgnoreCase) }).Count) {
                $plan.status = 'skipped'; $plan.reason = 'Version is still running.'; $plan; continue
            }
            if (@($ProtectedPaths | Where-Object { $_ -and ($_.TrimEnd('\') -eq $dir -or $_.StartsWith($dir + '\', [StringComparison]::OrdinalIgnoreCase)) }).Count) {
                $plan.status = 'skipped'; $plan.reason = 'The registered camera points into this folder.'; $plan; continue
            }
            # Walk with an explicit stack; never enter a reparse point (it retains the folder).
            $files = [Collections.Generic.List[string]]::new()
            $stack = [Collections.Generic.Stack[string]]::new()
            $stack.Push($dir)
            while ($stack.Count) {
                $here = $stack.Pop()
                foreach ($item in Get-ChildItem -LiteralPath $here -Force) {
                    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point: $($item.FullName)" }
                    if ($item.PSIsContainer) { $stack.Push($item.FullName) } else { $files.Add($item.FullName.Substring($dir.Length + 1)) }
                }
            }
            # The folder's own manifest proves payload only if it is valid and names this release.
            $proven = @{}
            $manifestPath = Join-Path $dir 'build-manifest.json'
            if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
                try {
                    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
                    if ($manifest.releaseId -eq $id -and $manifest.sourceCommit -match '^[a-f0-9]{40}$' -and @($manifest.files).Count) {
                        foreach ($entry in $manifest.files) {
                            $path = Get-OwnedInstallPath $dir $entry.path
                            if ($entry.sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid payload hash.' }
                            $proven[$path] = $entry.sha256
                        }
                    }
                } catch { $proven = @{} }   # an unusable manifest proves nothing: its files stay unknown
            }
            $runtime = @{}
            $runtimeManifest = Join-Path $dir 'notices\ffmpeg\installed-files.txt'
            if (Test-Path -LiteralPath $runtimeManifest -PathType Leaf) {
                foreach ($relative in Get-Content -LiteralPath $runtimeManifest) {
                    if ($relative -and ($relative -replace '[\\/]+', '/') -match $script:RuntimeFilePattern) { $runtime[(Get-OwnedInstallPath $dir $relative)] = $true }
                }
                $runtime[$runtimeManifest] = $true
            }
            $metadata = [Collections.Generic.List[string]]::new()
            $payload = [Collections.Generic.List[string]]::new()
            $unknown = [Collections.Generic.List[string]]::new()
            foreach ($relative in $files) {
                $path = Get-OwnedInstallPath $dir $relative
                if ($relative -notmatch '[\\/]' -and $metadataNames -contains $relative) { $metadata.Add($path) }
                elseif ($proven.ContainsKey($path) -and (Get-InstallPayloadSha256 -Path $path) -eq $proven[$path]) { $payload.Add($path) }
                elseif ($runtime.ContainsKey($path)) { $payload.Add($path) }
                else { $unknown.Add($path) }
            }
            $plan.metadata = @($metadata); $plan.payload = @($payload); $plan.unknown = @($unknown)
        } catch { $plan.status = 'retained'; $plan.reason = $_.Exception.Message; $plan.metadata = @(); $plan.payload = @(); $plan.unknown = @() }
        $plan
    }
}

# #883 + owner ruling D4 (2026-10-23): retire one residual folder from its plan. Unknown
# content (Recordings\, modified payload) is MOVED into a dated backup folder, never deleted;
# backup-manifest.json lists every planned move BEFORE the first one, so after a crash every
# file is either at its source or at a destination the manifest names. Then hash-proven
# payload and owned metadata are deleted one file at a time and empty directories removed
# deepest first, non-recursively. Any failure keeps the folder (status 'retained') and stops
# work on that folder only.
function New-ResidualBackupFolder {
    param([string]$BackupRoot)
    return Join-Path $BackupRoot ('retired-install-remnants-{0:yyyyMMdd-HHmmss}-{1}' -f [DateTime]::Now, [Guid]::NewGuid().ToString('N').Substring(0, 8))
}

function Write-ResidualBackupManifest {
    param([string]$BackupFolder, [object[]]$Entries)
    [void][IO.Directory]::CreateDirectory($BackupFolder)
    $json = [ordered]@{ purpose = 'CoreVideo Pro setup moved these operator files out of retired version folders (#883). Nothing was deleted.'; entries = @($Entries) } | ConvertTo-Json -Depth 5
    $path = Join-Path $BackupFolder 'backup-manifest.json'
    $temp = "$path.tmp"
    [IO.File]::WriteAllText($temp, $json, (New-Object Text.UTF8Encoding $false))
    if ([IO.File]::Exists($path)) { [IO.File]::Replace($temp, $path, [NullString]::Value) } else { [IO.File]::Move($temp, $path) }
}

function Remove-ResidualInstallation {
    param($Plan, [string]$BackupFolder, [switch]$LeaveUnknown,
        # Test hooks: inject a failure after N moves; pretend the backup is on another volume.
        [int]$FailAfterMoves = 0, [string]$BackupVolumeRoot = '')
    $Plan | Add-Member -NotePropertyName backedUpTo -NotePropertyValue '' -Force
    $Plan | Add-Member -NotePropertyName moved -NotePropertyValue 0 -Force
    if ($Plan.status -ne 'ready') { return $Plan }
    $dir = $Plan.directory
    try {
        $unknown = @($Plan.unknown)
        if ($unknown.Count -and $LeaveUnknown) {
            $Plan.status = 'retained'; $Plan.reason = "operator content: $($unknown.Count) files (left in place)"; return $Plan
        }
        if ($unknown.Count) {
            if (!$BackupFolder) { throw 'No backup folder given for operator content.' }
            $sourceVolume = [IO.Path]::GetPathRoot($dir)
            $backupVolume = if ($BackupVolumeRoot) { $BackupVolumeRoot } else { [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($BackupFolder)) }
            if ($sourceVolume -ine $backupVolume) { throw "Backup folder is on another volume ($backupVolume); operator content was not moved." }
            $manifestPath = Join-Path $BackupFolder 'backup-manifest.json'
            $entries = [Collections.Generic.List[object]]::new()
            if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
                foreach ($e in @((Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json).entries)) { $entries.Add($e) }
            }
            $taken = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($e in $entries) { [void]$taken.Add($e.destination) }
            $mine = [Collections.Generic.List[object]]::new()
            foreach ($source in $unknown) {
                $relative = $source.Substring($dir.Length + 1)
                $checked = Get-OwnedInstallPath $dir $relative
                $destination = Join-Path (Join-Path $BackupFolder $Plan.releaseId) $relative
                $base = [IO.Path]::Combine([IO.Path]::GetDirectoryName($destination), [IO.Path]::GetFileNameWithoutExtension($destination))
                $extension = [IO.Path]::GetExtension($destination)
                for ($n = 1; $taken.Contains($destination) -or [IO.File]::Exists($destination) -or [IO.Directory]::Exists($destination); $n++) { $destination = "$base ($n)$extension" }
                [void]$taken.Add($destination)
                $entry = [pscustomobject][ordered]@{ releaseId = $Plan.releaseId; source = $checked; destination = $destination
                    length = (Get-Item -LiteralPath $checked -Force).Length; sha256 = (Get-InstallPayloadSha256 -Path $checked); status = 'planned' }
                $entries.Add($entry); $mine.Add($entry)
            }
            # Manifest first: from here on every file is at its source or at a listed destination.
            Write-ResidualBackupManifest $BackupFolder $entries
            $Plan.backedUpTo = Join-Path $BackupFolder $Plan.releaseId
            try {
                foreach ($entry in $mine) {
                    if ($FailAfterMoves -and $Plan.moved -ge $FailAfterMoves) { throw "Injected failure after $($Plan.moved) moves." }
                    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($entry.destination))
                    [IO.File]::Move($entry.source, $entry.destination)   # same volume: a rename; never overwrites
                    if ((Get-InstallPayloadSha256 -Path $entry.destination) -ne $entry.sha256) { throw "Moved file changed: $($entry.destination)" }
                    $entry.status = 'moved'
                    $Plan.moved++
                }
            } finally { Write-ResidualBackupManifest $BackupFolder $entries }
        }
        # Payload first, owned metadata last (a retry still has the manifest).
        foreach ($path in @($Plan.payload) + @($Plan.metadata)) {
            $checked = Get-OwnedInstallPath $dir $path.Substring($dir.Length + 1)
            if (Test-Path -LiteralPath $checked -PathType Leaf) { Remove-Item -LiteralPath $checked -Force -ErrorAction Stop }
        }
        # Empty directories, deepest first, one at a time, never recursive.
        $directories = [Collections.Generic.List[string]]::new()
        $stack = [Collections.Generic.Stack[string]]::new(); $stack.Push($dir)
        while ($stack.Count) {
            $here = $stack.Pop(); $directories.Add($here)
            foreach ($item in Get-ChildItem -LiteralPath $here -Force -Directory) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point appeared: $($item.FullName)" }
                $stack.Push($item.FullName)
            }
        }
        foreach ($directory in $directories | Sort-Object Length -Descending) {
            if (!(Get-ChildItem -LiteralPath $directory -Force)) { Remove-Item -LiteralPath $directory -ErrorAction Stop }
        }
        if (Test-Path -LiteralPath $dir) {
            $left = @(Get-ChildItem -LiteralPath $dir -Force -Recurse -File | Select-Object -First 1)
            throw "Folder not empty after cleanup: $(if ($left) { $left[0].FullName } else { $dir })"
        }
        $Plan.status = if ($Plan.moved) { 'backed-up' } else { 'removed' }
    } catch { $Plan.status = 'retained'; $Plan.reason = $_.Exception.Message }
    return $Plan
}

function Remove-PreviousInstallation {
    param($Plan, [string]$ProgramsDirectory)
    if ($Plan.status -ne 'ready') { return $Plan }
    try {
        # Revalidate every path immediately before deletion. No recursive removal.
        $directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($path in $Plan.files) {
            $relative = $path.Substring($Plan.directory.Length + 1)
            $checked = Get-OwnedInstallPath $Plan.directory $relative
            if (Test-Path -LiteralPath $checked -PathType Leaf) { Remove-Item -LiteralPath $checked -Force -ErrorAction Stop }
            for ($parent = Split-Path -Parent $checked; $parent -and $parent.StartsWith($Plan.directory, [StringComparison]::OrdinalIgnoreCase); $parent = Split-Path -Parent $parent) {
                [void]$directories.Add($parent)
            }
        }
        if ($ProgramsDirectory) {
            $folder = Join-Path $ProgramsDirectory "CoreVideo Pro $($Plan.releaseId)"
            $shell = New-Object -ComObject WScript.Shell
            try {
                foreach ($name in @('CoreVideo Pro.lnk','Uninstall.lnk')) {
                    $link = Get-OwnedInstallPath $folder $name
                    if (Test-Path -LiteralPath $link -PathType Leaf) {
                        $shortcut = $shell.CreateShortcut($link)
                        try {
                            if ($shortcut.TargetPath.StartsWith($Plan.directory + '\', [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $link -Force }
                        } finally { [Runtime.InteropServices.Marshal]::ReleaseComObject($shortcut) | Out-Null }
                    }
                }
            } finally { [Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null }
            if ((Test-Path -LiteralPath $folder) -and !(Get-ChildItem -LiteralPath $folder -Force)) { Remove-Item -LiteralPath $folder }
        }
        # Retain marker/manifest/uninstaller if any payload operation failed, so
        # the user can retry. Metadata is retired only after payload completion.
        foreach ($relative in Get-OwnedInstallMetadataNames) {
            $path = Get-OwnedInstallPath $Plan.directory $relative
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force -ErrorAction Stop }
        }
        Remove-Item -LiteralPath $Plan.registryKey -ErrorAction Stop
        [void]$directories.Add($Plan.directory)
        foreach ($directory in $directories | Sort-Object Length -Descending) {
            if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -LiteralPath $directory -Force)) { Remove-Item -LiteralPath $directory -ErrorAction Stop }
        }
        $Plan.status = 'removed'
    } catch { $Plan.status = 'retained'; $Plan.reason = $_.Exception.Message }
    return $Plan
}

Export-ModuleMember -Function Get-OwnedInstallMetadataNames,Get-ResidualInstallationPlan,New-ResidualBackupFolder,Remove-ResidualInstallation,Initialize-SetupHelperEnvironment,Format-SetupHelperDiagnostics,Get-InstallPayloadSha256,Get-PreviousInstallationPlan,Remove-PreviousInstallation
