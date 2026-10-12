Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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
                    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $file.sha256) { $paths.Add($path) }
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
        foreach ($relative in @('Uninstall.exe','.camera-registration-before.json','.corevideo-prerelease-install','build-manifest.json')) {
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

Export-ModuleMember -Function Get-PreviousInstallationPlan,Remove-PreviousInstallation
