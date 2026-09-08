# Assemble a clean public alpha from an explicitly selected self-contained publish.
# Never copies a development app directory or modifies the desktop/running app.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ReleaseId,
    [Parameter(Mandatory=$true)][string]$PublishDirectory,
    [Parameter(Mandatory=$true)][string]$NativeBuildDirectory,
    [string]$NodeExe = ''
)
$ErrorActionPreference = 'Stop'
if ($ReleaseId -notmatch '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Use alpha-YYYY-MM-DD-build or beta-YYYY-MM-DD-build as ReleaseId.' }
$channel = $Matches[1].ToLowerInvariant()
$channelTitle = (Get-Culture).TextInfo.ToTitleCase($channel)
$repoRoot = Split-Path -Parent $PSScriptRoot
$publish = (Resolve-Path -LiteralPath $PublishDirectory).Path
$native = (Resolve-Path -LiteralPath $NativeBuildDirectory).Path
$nodePin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'alpha/node-runtime.json') -Raw | ConvertFrom-Json
if (-not $NodeExe) { $NodeExe = (Get-Command node -ErrorAction Stop).Source }
$NodeExe = (Resolve-Path -LiteralPath $NodeExe).Path
if ((Get-FileHash -LiteralPath $NodeExe -Algorithm SHA256).Hash -ne $nodePin.sha256) {
    throw "Alpha requires the pinned official Node $($nodePin.version) Windows x64 executable. Pass -NodeExe with that binary."
}
$output = Join-Path $repoRoot "artifacts/releases/$ReleaseId"
if (Test-Path -LiteralPath $output) { throw 'Release output already exists; choose a new immutable release ID.' }
$app = Join-Path $output "CoreVideoPro-$channelTitle"
New-Item -ItemType Directory -Path $app -Force | Out-Null
foreach ($file in @('CoreVideoPro.WinUI.exe','CoreVideoPro.WinUI.dll','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $file) -PathType Leaf)) { throw "Self-contained publish missing $file" }
}
foreach ($dir in @('Recordings','Logs','CrashReports','SupportBundles','publish','node','show-engine')) {
    if (Test-Path -LiteralPath (Join-Path $publish $dir)) { throw "Unexpected runtime/build data in publish: $dir" }
}
foreach ($file in @('sdk.dll','corevideo-zoom-engine.exe')) {
    if (Test-Path -LiteralPath (Join-Path $publish $file)) { throw "Publish contains a legacy root Zoom component: $file. Use a clean shell publish." }
}
Get-ChildItem -LiteralPath $publish | Where-Object { $_.Name -notlike 'runtime-probe*' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $app -Recurse -Force }
Get-ChildItem -LiteralPath $app -Recurse -File -Filter '*.pdb' | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
Copy-Item -LiteralPath (Join-Path $repoRoot 'native-shell/CoreVideoPro.WinUI/Assets') -Destination $app -Recurse -Force
# Reuse the normal host staging path, but pin its input and replace its private
# build-machine provenance with public upstream identity before ZIP creation.
& (Join-Path $PSScriptRoot 'sync-node-runtime-to-app.ps1') -AppDir $app -NodeExe $NodeExe
if (-not $?) { throw 'Node/show-engine staging failed.' }
$nodeManifest = [ordered]@{
    nodeVersion=$nodePin.version; nodeSha256=$nodePin.sha256
    nodeSource=$nodePin.url; entry='show-engine/dist/host/main.js'
}
$nodeManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $app 'corevideo-show-engine-runtime.json') -Encoding UTF8
$nativeFiles = @('corevideo-native.exe','corevideo-browser-host.exe','corevideo-plugin-host.exe','corevideo-virtualcam.dll')
foreach ($file in $nativeFiles) {
    $source = Join-Path $native $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Production native component missing: $file" }
    Copy-Item -LiteralPath $source -Destination $app
}
# Keep the Zoom process and SDK isolated from the app-local CRT used by WinUI
# and the main native core. Zoom uses the installed Microsoft x64 VC runtime.
# Do not use sync-zoom-runtime-to-app.ps1 here: it also flattens SDK files into
# the application root, which defeats this process dependency boundary.
$sdkRoot = Join-Path $repoRoot 'native-core/zoom-runtime/windows/x64'
$sdkSource = Join-Path $sdkRoot 'bin'
$sdkDestination = Join-Path $app 'zoom-runtime/windows/x64'
$sdkBin = Join-Path $app 'zoom-runtime/windows/x64/bin'
$crtFilePattern = '^(?i:msvcp[0-9].*|msvcr[0-9].*|vcruntime[0-9].*|concrt[0-9].*|vcomp[0-9].*|ucrtbase|api-ms-win-crt-.*)\.dll$'
if (-not (Test-Path -LiteralPath (Join-Path $sdkSource 'sdk.dll') -PathType Leaf)) {
    throw 'Stage the production Zoom SDK before packaging.'
}
# The managed readiness gate still requires these development files even for
# packaged joins. Preserve its architecture-root contract alongside isolated bin.
foreach ($required in @('lib/sdk.lib','h/zoom_sdk.h','h/meeting_service_interface.h',
    'h/rawdata/zoom_rawdata_api.h','h/rawdata/rawdata_renderer_interface.h','h/rawdata/rawdata_audio_helper_interface.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot $required) -PathType Leaf)) {
        throw "Staged Zoom SDK is missing a managed readiness prerequisite: $required"
    }
}
if (@(Get-ChildItem -LiteralPath $sdkSource -Recurse -File | Where-Object { $_.Name -match $crtFilePattern }).Count) {
    throw 'Staged Zoom SDK contains app-local CRT DLLs. Supply the isolated SDK without CRT copies; Zoom requires the installed Microsoft x64 VC runtime.'
}
New-Item -ItemType Directory -Path $sdkBin -Force | Out-Null
Get-ChildItem -LiteralPath $sdkSource -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $sdkBin -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $sdkRoot 'h') -Destination $sdkDestination -Recurse -Force
$sdkLib = Join-Path $sdkDestination 'lib'
New-Item -ItemType Directory -Path $sdkLib -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $sdkRoot 'lib/sdk.lib') -Destination $sdkLib -Force
$zoomHelper = Join-Path $native 'corevideo-zoom-engine.exe'
if (-not (Test-Path -LiteralPath $zoomHelper -PathType Leaf)) { throw 'Production Zoom helper is missing.' }
Copy-Item -LiteralPath $zoomHelper -Destination $sdkBin -Force
# Non-Zoom native helpers link the desktop VC runtime dynamically. Use the installed
# Visual Studio redistribution tree, never DLLs scavenged from System32/PATH.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio Installer vswhere.exe is required to locate licensed VC redistributables.' }
$vsPaths = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
$vsRoot = $vsPaths | Select-Object -First 1
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($vsRoot)) { throw 'No installed Visual Studio C++ redistributable location found.' }
$versionFile = Join-Path $vsRoot 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
$crtVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($crtVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Unexpected Visual C++ redistributable version.' }
$crtRoot = Join-Path $vsRoot "VC/Redist/MSVC/$crtVersion/x64"
$crtDirectories = @(Get-ChildItem -LiteralPath $crtRoot -Directory | Where-Object { $_.Name -match '^Microsoft\.VC[0-9]+\.CRT$' })
if ($crtDirectories.Count -ne 1) { throw 'Expected exactly one desktop x64 VC CRT redistributable directory.' }
$crtDirectory = $crtDirectories[0].FullName
foreach ($required in @('msvcp140.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $crtDirectory $required) -PathType Leaf)) { throw "VC redistributable is missing required native import: $required" }
}
Get-ChildItem -LiteralPath $crtDirectory -File -Filter '*.dll' |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $app -Force }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'alpha/Install-MediaRuntime.ps1') -Destination $app
$testerGuide = if ($channel -eq 'beta') { 'docs/beta-tester-guide.md' } else { 'docs/alpha-tester-guide.md' }
Copy-Item -LiteralPath (Join-Path $repoRoot $testerGuide) -Destination (Join-Path $app 'README.md')
$notices = Join-Path $app 'notices'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'docs/alpha-third-party-inputs') -File |
    Where-Object { $_.Name -notmatch '(?i)ffmpeg' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $notices }
@'
@echo off
setlocal
cd /d "%~dp0"
if not exist "notices\ffmpeg\download-provenance.txt" goto install
for %%F in (ffmpeg.exe avcodec-62.dll avformat-62.dll avutil-60.dll swscale-9.dll swresample-6.dll) do if not exist "%%F" goto install
goto launch
:install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MediaRuntime.ps1" -AppDirectory "%~dp0."
if errorlevel 1 (
  echo Media setup did not complete. Keep this message for your bug report.
  pause
  exit /b 1
)
:launch
start "" "%~dp0CoreVideoPro.WinUI.exe" %*
'@ | Set-Content -LiteralPath (Join-Path $app 'StartCoreVideo.cmd') -Encoding ASCII
@'
@echo off
setlocal
cd /d "%~dp0"
"%SystemRoot%\System32\regsvr32.exe" "%~dp0corevideo-virtualcam.dll"
'@ | Set-Content -LiteralPath (Join-Path $app 'Register-VirtualCamera.cmd') -Encoding ASCII
@'
@echo off
setlocal
cd /d "%~dp0"
"%SystemRoot%\System32\regsvr32.exe" /u "%~dp0corevideo-virtualcam.dll"
'@ | Set-Content -LiteralPath (Join-Path $app 'Unregister-VirtualCamera.cmd') -Encoding ASCII
$files = @(Get-ChildItem -LiteralPath $app -Recurse -File)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($app.Length + 1).Replace('\','/')
    if ($relative -match '^show-engine/dist/' -and
        ($relative -notmatch '\.(js|d\.ts)$' -or $relative -match '(?i)(^|/)(tests?|fixtures?|__[^/]+__)(/|$)|\.(test|spec)\.|\.map$')) {
        throw "Development or unexpected show-engine payload: $relative. Rebuild show-engine from a clean dist directory."
    }
    if ($relative -match '^zoom-runtime/windows/x64/bin/' -and $file.Name -match $crtFilePattern) {
        throw "App-local CRT is forbidden inside the isolated Zoom SDK: $relative"
    }
    if ($relative -in @('sdk.dll','corevideo-zoom-engine.exe')) { throw "Legacy root Zoom component is forbidden: $relative" }
    if ($relative -match '(?i)(^|/)(Recordings|Logs|CrashReports|SupportBundles)(/|$)|-fake\.exe$|-tests\.exe$|\.pdb$|\.dmp$|(^|/)(production-output-preferences|zoom-oauth)|(^|/)(ffmpeg|ffprobe|ffplay)\.exe$|(^|/)(av(codec|format|util|device|filter)|swscale|swresample|postproc)-[0-9]+\.dll$') {
        throw "Disallowed public prerelease content: $relative"
    }
}
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
$manifest = [ordered]@{
    releaseId=$ReleaseId; sourceCommit=$commit; platform='Windows x64'; channel=$channel; signed=$false
    appRuntime='Bundled .NET, Windows App SDK and app-local Visual C++ CRT for shell/native core'; vcRuntimeVersion=$crtVersion; mediaRuntime='Verified upstream download on first launch'
    zoomRuntime='Isolated SDK/helper; requires installed Microsoft Visual C++ v14 x64 Redistributable'
    framePerformanceAccepted=$false
    files=@($files | Sort-Object FullName | ForEach-Object { [ordered]@{
        path=$_.FullName.Substring($app.Length+1).Replace('\','/'); bytes=$_.Length
        sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }})
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $app 'build-manifest.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $output "CoreVideoPro-win-x64-$ReleaseId.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($app, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zipPath))" | Set-Content -LiteralPath ($zipPath + '.sha256') -Encoding ASCII
[pscustomobject]@{ package=$zipPath; sha256=$hash; sourceCommit=$commit } | ConvertTo-Json
