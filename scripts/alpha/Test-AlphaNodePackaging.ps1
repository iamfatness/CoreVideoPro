# Exercises the real ZIP validator with inert package entries; never launches an app.
[CmdletBinding()]
param([string]$NodeExe = (Get-Command node -ErrorAction Stop).Source)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$pin = Get-Content (Join-Path $PSScriptRoot 'node-runtime.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $NodeExe).Hash -ne $pin.sha256) { throw 'Test requires the pinned Node binary.' }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('corevideo-alpha-node-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
$required = @('CoreVideoPro.WinUI.exe','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll','Microsoft.WinUI.dll',
    'corevideo-native.exe','zoom-runtime/windows/x64/bin/corevideo-zoom-engine.exe','zoom-runtime/windows/x64/bin/sdk.dll',
    'Assets/AppIcon.ico','README.md','zoom-runtime/windows/x64/lib/sdk.lib','zoom-runtime/windows/x64/h/zoom_sdk.h',
    'zoom-runtime/windows/x64/h/meeting_service_interface.h','zoom-runtime/windows/x64/h/rawdata/zoom_rawdata_api.h',
    'zoom-runtime/windows/x64/h/rawdata/rawdata_renderer_interface.h','zoom-runtime/windows/x64/h/rawdata/rawdata_audio_helper_interface.h',
    'StartCoreVideo.cmd','Install-MediaRuntime.ps1','msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll',
    'show-engine/dist/host/main.js','show-engine/package.json',"notices/$($pin.license)",'App.xbf','Assets/Fonts/Test.ttf')
$cases = @(
    @{ name='valid'; error='' },
    @{ name='missing-node'; remove='node/node.exe'; error='Missing package component: node/node.exe' },
    @{ name='missing-host'; remove='show-engine/dist/host/main.js'; error='Missing package component: show-engine/dist/host/main.js' },
    @{ name='missing-notice'; remove="notices/$($pin.license)"; error='Missing package component: notices/' },
    @{ name='wrong-node'; error='Node executable does not match' },
    @{ name='private-provenance'; error='Invalid or private Node runtime provenance' },
    @{ name='test-fixture'; extra='show-engine/dist/fixtures/private.js'; error='Development or unexpected show-engine payload' },
    @{ name='source-map'; extra='show-engine/dist/host/main.js.map'; error='Development or unexpected show-engine payload' }
)
try {
    foreach ($case in $cases) {
        $files = @{}
        foreach ($name in $required) { $files[$name] = $utf8.GetBytes('inert validator fixture') }
        $files['node/node.exe'] = [IO.File]::ReadAllBytes($NodeExe)
        if ($case.name -eq 'wrong-node') { $files['node/node.exe'] = $utf8.GetBytes('incorrect runtime') }
        $source = $pin.url
        if ($case.name -eq 'private-provenance') { $source = 'C:\Users\private\node.exe' }
        $files['corevideo-show-engine-runtime.json'] = $utf8.GetBytes((@{
            nodeVersion=$pin.version; nodeSha256=$pin.sha256; nodeSource=$source; entry='show-engine/dist/host/main.js'
        } | ConvertTo-Json))
        if ($case.remove) { $files.Remove($case.remove) }
        if ($case.extra) { $files[$case.extra] = $utf8.GetBytes('private development fixture') }
        $inventory = @($files.Keys | ForEach-Object {
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [BitConverter]::ToString($sha.ComputeHash($files[$_])).Replace('-','').ToLowerInvariant() }
            finally { $sha.Dispose() }
            @{path=$_; bytes=$files[$_].Length; sha256=$hash}
        })
        $files['build-manifest.json'] = $utf8.GetBytes((@{channel='alpha'; framePerformanceAccepted=$false; files=$inventory} | ConvertTo-Json -Depth 6))
        $archive = Join-Path $testRoot ($case.name + '.zip')
        $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($name in $files.Keys) {
                $entry = $zip.CreateEntry('CoreVideoPro-Alpha/' + $name, [IO.Compression.CompressionLevel]::NoCompression)
                $stream = $entry.Open()
                try { $stream.Write($files[$name], 0, $files[$name].Length) } finally { $stream.Dispose() }
            }
        } finally { $zip.Dispose() }
        (Get-FileHash -LiteralPath $archive).Hash | Set-Content -LiteralPath ($archive + '.sha256')
        $failure = ''
        try { & (Join-Path $PSScriptRoot 'Test-AlphaPackage.ps1') -Archive $archive | Out-Null }
        catch { $failure = $_.Exception.Message }
        if ($case.error -and -not $failure.StartsWith($case.error)) { throw "$($case.name): unexpected result '$failure'" }
        if (-not $case.error -and $failure) { throw "$($case.name): $failure" }
        Write-Host "PASS $($case.name)"
        Remove-Item -LiteralPath $archive,($archive + '.sha256')
    }
    Write-Host "$($cases.Count) alpha Node packaging checks passed."
} finally {
    # The directory is a freshly created, absolute, per-run child of TEMP.
    if ([IO.Path]::GetDirectoryName($testRoot).TrimEnd('\') -eq ([IO.Path]::GetTempPath()).TrimEnd('\')) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
