[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Archive)
$ErrorActionPreference = 'Stop'
$nodePin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'node-runtime.json') -Raw | ConvertFrom-Json
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
        if ($relative -match '^show-engine/dist/' -and
            ($relative -notmatch '\.(js|d\.ts)$' -or $relative -match '(?i)(^|/)(tests?|fixtures?|__[^/]+__)(/|$)|\.(test|spec)\.|\.map$')) {
            throw "Development or unexpected show-engine payload: $relative"
        }
        if ($relative -in @('sdk.dll','corevideo-zoom-engine.exe')) { throw "Legacy root Zoom component is forbidden: $relative" }
        if ($relative -match '(?i)^zoom-runtime/windows/x64/bin/(?:.*/)?(?:msvcp[0-9].*|msvcr[0-9].*|vcruntime[0-9].*|concrt[0-9].*|vcomp[0-9].*|ucrtbase|api-ms-win-crt-.*)\.dll$') {
            throw "App-local CRT is forbidden inside the isolated Zoom SDK: $relative"
        }
        if ($entries.ContainsKey($relative)) { throw 'Duplicate archive entry.' }
        if ($relative -match '(?i)(^|/)(Recordings|Logs|CrashReports|SupportBundles|runtime-probe[^/]*)(/|$)|-fake\.exe$|-tests\.exe$|\.pdb$|\.dmp$|(^|/)(production-output-preferences|zoom-oauth)|(^|/)(ffmpeg|ffprobe|ffplay)\.exe$|(^|/)(av(codec|format|util|device|filter)|swscale|swresample|postproc)-[0-9]+\.dll$') { throw "Unexpected runtime/development data: $relative" }
        $entries[$relative] = $entry
    }
    foreach ($required in @('CoreVideoPro.WinUI.exe','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','Microsoft.WinUI.dll',
        'corevideo-native.exe','zoom-runtime/windows/x64/bin/corevideo-zoom-engine.exe','zoom-runtime/windows/x64/bin/sdk.dll','Assets/AppIcon.ico','README.md',
        'zoom-runtime/windows/x64/lib/sdk.lib','zoom-runtime/windows/x64/h/zoom_sdk.h',
        'zoom-runtime/windows/x64/h/meeting_service_interface.h','zoom-runtime/windows/x64/h/rawdata/zoom_rawdata_api.h',
        'zoom-runtime/windows/x64/h/rawdata/rawdata_renderer_interface.h','zoom-runtime/windows/x64/h/rawdata/rawdata_audio_helper_interface.h',
        'StartCoreVideo.cmd','Install-MediaRuntime.ps1','build-manifest.json',
        'msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll',
        'node/node.exe','show-engine/dist/host/main.js','show-engine/package.json',
        'corevideo-show-engine-runtime.json',"notices/$($nodePin.license)")) {
        if (-not $entries.ContainsKey($required)) { throw "Missing package component: $required" }
    }
    if (-not @($entries.Keys | Where-Object { $_ -like '*.xbf' }).Count) { throw 'Compiled XAML is missing.' }
    $nodeStream = $entries['node/node.exe'].Open(); $nodeSha = [Security.Cryptography.SHA256]::Create()
    try { $nodeHash = [BitConverter]::ToString($nodeSha.ComputeHash($nodeStream)).Replace('-','').ToLowerInvariant() }
    finally { $nodeSha.Dispose(); $nodeStream.Dispose() }
    if ($nodeHash -ne $nodePin.sha256) { throw 'Node executable does not match the pinned upstream runtime.' }
    $nodeReader = [IO.StreamReader]::new($entries['corevideo-show-engine-runtime.json'].Open())
    try { $runtime = $nodeReader.ReadToEnd() | ConvertFrom-Json } finally { $nodeReader.Dispose() }
    if ($runtime.nodeSource -ne $nodePin.url -or $runtime.nodeVersion -ne $nodePin.version -or
        $runtime.nodeSha256 -ne $nodePin.sha256 -or $runtime.entry -ne 'show-engine/dist/host/main.js') {
        throw 'Invalid or private Node runtime provenance.'
    }
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
