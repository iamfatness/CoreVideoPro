# Assemble a clean public alpha from an explicitly selected self-contained publish.
# Never copies a development app directory or modifies the desktop/running app.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ReleaseId,
    [Parameter(Mandatory=$true)][string]$PublishDirectory,
    [Parameter(Mandatory=$true)][string]$NativeBuildDirectory
)
$ErrorActionPreference = 'Stop'
if ($ReleaseId -notmatch '^alpha-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Use alpha-YYYY-MM-DD-build as ReleaseId.' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$publish = (Resolve-Path -LiteralPath $PublishDirectory).Path
$native = (Resolve-Path -LiteralPath $NativeBuildDirectory).Path
$output = Join-Path $repoRoot "artifacts/releases/$ReleaseId"
if (Test-Path -LiteralPath $output) { throw 'Release output already exists; choose a new immutable release ID.' }
$app = Join-Path $output 'CoreVideoPro-Alpha'
New-Item -ItemType Directory -Path $app -Force | Out-Null
foreach ($file in @('CoreVideoPro.WinUI.exe','CoreVideoPro.WinUI.dll','coreclr.dll','hostfxr.dll','Microsoft.UI.Xaml.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $file) -PathType Leaf)) { throw "Self-contained publish missing $file" }
}
foreach ($dir in @('Recordings','Logs','CrashReports','SupportBundles','publish')) {
    if (Test-Path -LiteralPath (Join-Path $publish $dir)) { throw "Unexpected runtime/build data in publish: $dir" }
}
Get-ChildItem -LiteralPath $publish | Where-Object { $_.Name -notlike 'runtime-probe*' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $app -Recurse -Force }
Get-ChildItem -LiteralPath $app -Recurse -File -Filter '*.pdb' | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
Copy-Item -LiteralPath (Join-Path $repoRoot 'native-shell/CoreVideoPro.WinUI/Assets') -Destination $app -Recurse -Force
$nativeFiles = @('corevideo-native.exe','corevideo-zoom-engine.exe','corevideo-browser-host.exe','corevideo-plugin-host.exe','corevideo-virtualcam.dll')
foreach ($file in $nativeFiles) {
    $source = Join-Path $native $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Production native component missing: $file" }
    Copy-Item -LiteralPath $source -Destination $app
}
# The existing SDK copier protects WinUI-owned resources. Its destination is a
# new release tree, so it cannot remove or overwrite a running app's runtime.
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'native-core/zoom-runtime/windows/x64/bin/sdk.dll'))) {
    throw 'Stage the production Zoom SDK before packaging.'
}
& (Join-Path $PSScriptRoot 'sync-zoom-runtime-to-app.ps1') -AppDir $app -RuntimeDir (Join-Path $repoRoot 'native-core/zoom-runtime/windows/x64')
# Native helpers link the desktop VC runtime dynamically. Use the installed
# Visual Studio redistribution tree, never DLLs scavenged from System32/PATH.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Visual Studio Installer vswhere.exe is required to locate licensed VC redistributables.' }
$vsRoot = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
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
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs/alpha-tester-guide.md') -Destination (Join-Path $app 'README.md')
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
    if ($relative -match '(?i)(^|/)(Recordings|Logs|CrashReports|SupportBundles)(/|$)|-fake\.exe$|-tests\.exe$|\.pdb$|\.dmp$|(^|/)(production-output-preferences|zoom-oauth)|(^|/)(ffmpeg|ffprobe|ffplay)\.exe$|(^|/)(av(codec|format|util|device|filter)|swscale|swresample|postproc)-[0-9]+\.dll$') {
        throw "Disallowed public alpha content: $relative"
    }
}
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
$manifest = [ordered]@{
    releaseId=$ReleaseId; sourceCommit=$commit; platform='Windows x64'; channel='alpha'; signed=$false
    appRuntime='Bundled .NET, Windows App SDK and Visual C++ CRT'; vcRuntimeVersion=$crtVersion; mediaRuntime='Verified upstream download on first launch'
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
