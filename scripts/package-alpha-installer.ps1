[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$VcRedist,
    [string]$MakeNsis = 'C:/Program Files (x86)/NSIS/makensis.exe',
    # Short, user-owned staging root (#863/#888). NSIS cannot open files whose
    # absolute path exceeds MAX_PATH, so staging must never inherit a long checkout path.
    [string]$StagingRoot = (Join-Path (Join-Path $env:LOCALAPPDATA 'Temp\cvp-pkg') ([Guid]::NewGuid().ToString('N').Substring(0, 8))),
    [int]$MaxStagedPathLength = 240
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
$work = [IO.Path]::GetFullPath($StagingRoot)
if ((Test-Path -LiteralPath $work) -and @(Get-ChildItem -LiteralPath $work -Force).Count -gt 0) {
    throw "Staging root is not empty; refusing to reuse it: $work"
}
New-Item -ItemType Directory -Path $work -Force | Out-Null
$evidence = Join-Path $repoRoot ('artifacts/installer-build/' + (Split-Path -Leaf $work))
$succeeded = $false
try {
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Check before extracting: Windows PowerShell 5.1 cannot create over-long paths at all.
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $deepest = $zip.Entries | Sort-Object { $_.FullName.Length } -Descending | Select-Object -First 1
    $deepestPath = Join-Path $work ($deepest.FullName.Replace('/', [string][IO.Path]::DirectorySeparatorChar))
    if ($deepestPath.Length -gt $MaxStagedPathLength) {
        throw "Staged path would be $($deepestPath.Length) characters (limit $MaxStagedPathLength); NSIS would fail to open it. Use a shorter -StagingRoot. Offending path: $deepestPath"
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $work)
$longest = Get-ChildItem -LiteralPath $work -Recurse -Force | Sort-Object { $_.FullName.Length } -Descending | Select-Object -First 1
if ($longest -and $longest.FullName.Length -gt $MaxStagedPathLength) {
    throw "Staged path is $($longest.FullName.Length) characters (limit $MaxStagedPathLength); NSIS would fail to open it. Use a shorter -StagingRoot. Offending path: $($longest.FullName)"
}
Write-Host "Longest staged path: $($longest.FullName.Length) characters"
$payload = @(Get-ChildItem -LiteralPath $work -Directory | Where-Object { $_.Name -match '^CoreVideoPro-(Alpha|Beta)$' })
if ($payload.Count -ne 1) { throw 'Archive must contain exactly one CoreVideoPro Alpha or Beta payload root.' }
$payload = $payload[0].FullName
$manifest = Get-Content -LiteralPath (Join-Path $payload 'build-manifest.json') -Raw | ConvertFrom-Json
$releaseId = $manifest.releaseId
if ($releaseId -notmatch '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Invalid release ID.' }
$channel = $Matches[1].ToLowerInvariant()
$channelTitle = (Get-Culture).TextInfo.ToTitleCase($channel)
if ((Split-Path -Leaf $payload) -ne "CoreVideoPro-$channelTitle") { throw 'Payload root does not match the manifest channel.' }
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
    '!define CHANNEL "' + $channel + '"'
    '!define CHANNEL_TITLE "' + $channelTitle + '"'
    '!define PAYLOAD "' + (Escape-Nsis $payload) + '"'
    '!define OUTPUT "' + (Escape-Nsis $output) + '"'
    '!define VC_REDIST "' + (Escape-Nsis $redistPath) + '"'
    '!define VC_MINIMUM "' + $manifest.vcRuntimeVersion + '"'
    '!define INSTALL_KB ' + [math]::Ceiling(($files | Measure-Object Length -Sum).Sum / 1KB)
)
[IO.File]::WriteAllLines((Join-Path $work 'generated.nsh'), $defines)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'alpha/installer.nsi') -Destination $work
$ErrorActionPreference = 'Continue'
$nsisOutput = @(& $MakeNsis /V2 (Join-Path $work 'installer.nsi') 2>&1 | ForEach-Object { "$_" })
$nsisExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$nsisOutput + "makensis exit code: $nsisExit" | Set-Content -LiteralPath (Join-Path $work 'makensis.log') -Encoding UTF8
$nsisOutput | Write-Host
if ($nsisExit -ne 0) { throw 'NSIS compilation failed.' }
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
$succeeded = $true
} finally {
    # Evidence stays where it always was: artifacts/installer-build/<id>.
    New-Item -ItemType Directory -Path $evidence -Force | Out-Null
    foreach ($log in @('makensis.log', 'generated.nsh', 'install-files.nsh', 'uninstall-files.nsh')) {
        if (Test-Path -LiteralPath (Join-Path $work $log)) { Copy-Item -LiteralPath (Join-Path $work $log) -Destination $evidence -Force }
    }
    if ($succeeded) { Remove-Item -LiteralPath $work -Recurse -Force }
    else { Write-Warning "Packaging failed; staging folder kept for inspection: $work (evidence: $evidence)" }
}
