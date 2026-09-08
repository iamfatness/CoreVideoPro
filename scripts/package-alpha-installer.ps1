[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$VcRedist,
    [string]$MakeNsis = 'C:/Program Files (x86)/NSIS/makensis.exe'
)
$ErrorActionPreference = 'Stop'
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
& (Join-Path $PSScriptRoot 'alpha/Test-AlphaPackage.ps1') -Archive $archivePath
if (-not $?) { throw 'Alpha archive validation failed.' }
$redistPath = (Resolve-Path -LiteralPath $VcRedist).Path
$signature = Get-AuthenticodeSignature -LiteralPath $redistPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
    throw 'VC redistributable must have a valid Microsoft signature.'
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$work = Join-Path $repoRoot ('artifacts/installer-build/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $work)
$payload = Join-Path $work 'CoreVideoPro-Alpha'
$manifest = Get-Content -LiteralPath (Join-Path $payload 'build-manifest.json') -Raw | ConvertFrom-Json
$releaseId = $manifest.releaseId
if ($releaseId -notmatch '^alpha-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Invalid release ID.' }
$redistVersion = [version]((Get-Item -LiteralPath $redistPath).VersionInfo.FileVersion -replace '[^0-9.].*$', '')
if ($redistVersion -lt [version]$manifest.vcRuntimeVersion) { throw 'VC redistributable is older than the packaged native runtime.' }
$output = Join-Path (Split-Path -Parent $archivePath) "CoreVideoPro-Setup-$releaseId.exe"
if (Test-Path -LiteralPath $output) { throw 'Installer already exists; do not replace an immutable release asset.' }
function Escape-Nsis([string]$value) { return $value.Replace('$', '$$').Replace('"', '$\"') }
$files = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName)
$install = [Collections.Generic.List[string]]::new()
$uninstall = [Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    $relative = $file.FullName.Substring($payload.Length + 1)
    $directory = Split-Path -Parent $relative
    $install.Add('SetOutPath "$INSTDIR\' + (Escape-Nsis $directory) + '"')
    $install.Add('IfErrors install_failed')
    $install.Add('File "' + (Escape-Nsis $file.FullName) + '"')
    $install.Add('IfErrors install_failed')
    $uninstall.Add('!insertmacro DeleteOwned "' + (Escape-Nsis $relative) + '"')
}
$directories = @(Get-ChildItem -LiteralPath $payload -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending)
foreach ($directory in $directories) {
    $uninstall.Add('RMDir "$INSTDIR\' + (Escape-Nsis $directory.FullName.Substring($payload.Length + 1)) + '"')
}
# RMDir is deliberately nonrecursive: downloaded media, recordings, and any other
# files created after installation are retained, as are all per-user preferences.
[IO.File]::WriteAllLines((Join-Path $work 'install-files.nsh'), $install)
[IO.File]::WriteAllLines((Join-Path $work 'uninstall-files.nsh'), $uninstall)
$defines = @(
    '!define RELEASE_ID "' + $releaseId + '"'
    '!define PAYLOAD "' + (Escape-Nsis $payload) + '"'
    '!define OUTPUT "' + (Escape-Nsis $output) + '"'
    '!define VC_REDIST "' + (Escape-Nsis $redistPath) + '"'
    '!define VC_MINIMUM "' + $manifest.vcRuntimeVersion + '"'
    '!define INSTALL_KB ' + [math]::Ceiling(($files | Measure-Object Length -Sum).Sum / 1KB)
)
[IO.File]::WriteAllLines((Join-Path $work 'generated.nsh'), $defines)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'alpha/installer.nsi') -Destination $work
& $MakeNsis /V2 (Join-Path $work 'installer.nsi')
if ($LASTEXITCODE -ne 0) { throw 'NSIS compilation failed.' }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($output))" | Set-Content -LiteralPath ($output + '.sha256') -Encoding ASCII
[ordered]@{
    releaseId=$releaseId; applicationCommit=$manifest.sourceCommit
    packagingCommit=(& git -C $repoRoot rev-parse HEAD).Trim()
    archiveSha256=(Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    installerSha256=$hash; vcRedistVersion=$redistVersion.ToString()
    vcRedistSha256=(Get-FileHash -LiteralPath $redistPath -Algorithm SHA256).Hash.ToLowerInvariant()
    signed=$false; scope='current user'; preservesUserData=$true
} | ConvertTo-Json | Set-Content -LiteralPath ($output + '.manifest.json') -Encoding UTF8
Write-Output $output
