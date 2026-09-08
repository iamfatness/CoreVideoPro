# App-local, opt-in acquisition of an upstream binary; no PATH/registry changes.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$AppDirectory)
$ErrorActionPreference = 'Stop'
$archiveName = 'ffmpeg-N-124714-g49a77d37be-win64-lgpl-shared.zip'
$url = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-05-31-13-22/' + $archiveName
$expectedHash = '56f4a1d367e9537f63849e5cf9103824f6d87f4fc39a6a22b717b4df186da054'
$appRoot = (Resolve-Path -LiteralPath $AppDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $appRoot 'CoreVideoPro.WinUI.exe') -PathType Leaf)) { throw 'AppDirectory must contain CoreVideoPro.WinUI.exe.' }
if ((Get-Item -LiteralPath $appRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'AppDirectory must not be a reparse point.' }
if (Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($appRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }) { throw 'Close this app before installing its media runtime.' }
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ('corevideo-media-install-' + [Guid]::NewGuid().ToString('N'))
$cache = Join-Path $tempRoot ('corevideo-media-' + $expectedHash + '.zip')
function Get-CheckedTarget([string]$relative) {
    $target = [IO.Path]::GetFullPath((Join-Path $appRoot $relative))
    if (-not $target.StartsWith($appRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Archive target escapes AppDirectory.' }
    $cursor = $target
    while ($cursor -and $cursor -ne $appRoot) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime target contains a reparse point.' }
        $cursor = Split-Path -Parent $cursor
    }
    return $target
}
New-Item -ItemType Directory -Path $work | Out-Null
$zip = $null
try {
    if (-not (Test-Path -LiteralPath $cache) -or (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ne $expectedHash) {
        $download = Join-Path $work $archiveName
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host 'Downloading pinned FFmpeg runtime (approximately 89 MB)...'
        Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $expectedHash) { throw 'FFmpeg archive SHA-256 mismatch; nothing installed.' }
        Copy-Item -LiteralPath $download -Destination $cache -Force
    }
    if ((Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Cached FFmpeg archive failed verification.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($cache)
    $prefix = $archiveName.Substring(0, $archiveName.Length - 4) + '/'
    $selected = @()
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if ($name -match '(^/|^[A-Za-z]:|(^|/)\.\.(/|$))') { throw 'Unsafe archive path.' }
        if (-not $name.StartsWith($prefix, [StringComparison]::Ordinal)) { throw 'Unexpected archive root.' }
        $relative = $name.Substring($prefix.Length)
        if ($relative -match '^bin/([^/]+\.dll|ffmpeg\.exe)$') {
            $leaf = $Matches[1]
            $selected += [pscustomobject]@{ Entry = $entry; Relative = $leaf }
        } elseif ($relative -match '^(LICENSE[^/]*|COPYING[^/]*|NOTICE[^/]*|README[^/]*)$') {
            $selected += [pscustomobject]@{ Entry = $entry; Relative = ('notices/ffmpeg/' + $relative) }
        }
    }
    # ABI majors required by the bundled native/media runtime.
    foreach ($required in @('ffmpeg.exe', 'avcodec-62.dll', 'avformat-62.dll', 'avutil-60.dll', 'swscale-9.dll', 'swresample-6.dll')) {
        if (@($selected | Where-Object { $_.Relative -eq $required }).Count -ne 1) { throw "Missing or duplicate required ABI file: $required" }
    }
    if (-not ($selected | Where-Object { $_.Relative -match '^notices/ffmpeg/LICENSE' })) { throw 'Archive has no license text.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in $selected) {
        $target = Get-CheckedTarget $item.Relative
        if (-not $seen.Add($target)) { throw 'Duplicate archive target.' }
        # Validate and stage everything before changing the app folder.
        $stage = Join-Path $work $item.Relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $stage) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($item.Entry, $stage, $false)
    }
    foreach ($item in $selected) {
        $target = Get-CheckedTarget $item.Relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $work $item.Relative) -Destination $target -Force
    }
    $provenance = Get-CheckedTarget 'notices/ffmpeg/download-provenance.txt'
    @("Upstream URL: $url", "Archive SHA-256: $expectedHash", 'Library variant: Windows x64 LGPL shared',
        'Installed app-locally; no global PATH changes.') | Set-Content -LiteralPath $provenance -Encoding UTF8
    Write-Host 'Verified FFmpeg media runtime installed beside CoreVideo Pro.'
} finally {
    if ($zip) { $zip.Dispose() }
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if ($resolvedWork.StartsWith($tempRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedWork) -match '^corevideo-media-install-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
