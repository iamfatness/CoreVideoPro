[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Archive)
$ErrorActionPreference = 'Stop'
$path = (Resolve-Path -LiteralPath $Archive).Path
$expected = ((Get-Content -LiteralPath ($path + '.sha256') -Raw).Trim() -split '\s+')[0]
if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) { throw 'Package checksum mismatch.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($path)
try {
    $prefix = 'CoreVideoPro-Alpha/'
    $entries = @{}
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\','/')
        if (-not $name.StartsWith($prefix) -or $name -match '(^|/)\.\.(/|$)') { throw 'Unexpected archive path.' }
        if ($name.EndsWith('/')) { continue }
        $relative = $name.Substring($prefix.Length)
        if ($entries.ContainsKey($relative)) { throw 'Duplicate archive entry.' }
        if ($relative -match '(?i)(^|/)(Recordings|Logs|CrashReports|SupportBundles|runtime-probe[^/]*)(/|$)|-fake\.exe$|-tests\.exe$|\.pdb$|\.dmp$|^ffmpeg\.exe$|^av(codec|format|util)-[0-9]+\.dll$') { throw "Unexpected runtime/development data: $relative" }
        $entries[$relative] = $entry
    }
    foreach ($required in @('CoreVideoPro.WinUI.exe','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','Microsoft.WinUI.dll',
        'corevideo-native.exe','corevideo-zoom-engine.exe','sdk.dll','Assets/AppIcon.ico','README.md',
        'StartCoreVideo.cmd','Install-MediaRuntime.ps1','build-manifest.json')) {
        if (-not $entries.ContainsKey($required)) { throw "Missing package component: $required" }
    }
    if (-not @($entries.Keys | Where-Object { $_ -like '*.xbf' }).Count) { throw 'Compiled XAML is missing.' }
    if (-not @($entries.Keys | Where-Object { $_ -like 'Assets/Fonts/*.ttf' }).Count) { throw 'Bundled fonts are missing.' }
    $reader = [IO.StreamReader]::new($entries['build-manifest.json'].Open())
    try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($manifest.channel -ne 'alpha' -or $manifest.framePerformanceAccepted -ne $false) { throw 'Incorrect release/acceptance label.' }
    if ($entries.Count -ne $manifest.files.Count + 1) { throw 'Manifest coverage mismatch.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        if (-not $seen.Add($file.path) -or -not $entries.ContainsKey($file.path)) { throw 'Missing or duplicated manifest file.' }
        $entry = $entries[$file.path]
        if ($entry.Length -ne $file.bytes) { throw "Length mismatch: $($file.path)" }
        $stream = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($actual -ne $file.sha256) { throw "File checksum mismatch: $($file.path)" }
    }
    [pscustomobject]@{ valid=$true; releaseId=$manifest.releaseId; sourceCommit=$manifest.sourceCommit; files=$entries.Count; sha256=$expected } | ConvertTo-Json
} finally { $zip.Dispose() }
