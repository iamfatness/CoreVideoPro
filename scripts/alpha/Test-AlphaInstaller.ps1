# Exercises a real silent install/uninstall in a new artifact directory. It never
# launches the product App or changes an existing installation or user settings.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$releaseId = [IO.Path]::GetFileNameWithoutExtension($installerPath).Replace('CoreVideoPro-Setup-', '')
if ($releaseId -notmatch '^alpha-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Unexpected installer name.' }
$registryPath = "HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/CoreVideoPro-$releaseId"
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) "CoreVideo Pro $releaseId.lnk"
$startFolder = Join-Path ([Environment]::GetFolderPath('Programs')) "CoreVideo Pro $releaseId"
if ((Test-Path $registryPath) -or (Test-Path -LiteralPath $desktopShortcut) -or (Test-Path -LiteralPath $startFolder)) {
    throw 'This release already has installation metadata; refusing to alter it for testing.'
}
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = Join-Path $repoRoot ('artifacts/installer-validation/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$installRoot = Join-Path $work 'installed app'
function Invoke-OwnedProcess([string]$executable, [string[]]$arguments) {
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(180000)) {
        Stop-Process -Id $process.Id
        throw 'Owned installer test process timed out.'
    }
    $process.Refresh()
    return $process.ExitCode
}
# /D is deliberately last and unquoted: NSIS treats everything after it as the path.
if ((Invoke-OwnedProcess $installerPath @('/S', "/D=$installRoot")) -ne 0) { throw "Silent installation failed. Evidence: $work" }
$registration = Get-ItemProperty $registryPath
if ($registration.InstallLocation -ne $installRoot) { throw 'Uninstall registration points at the wrong directory.' }
$manifest = Get-Content -LiteralPath (Join-Path $installRoot 'build-manifest.json') -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    if ((Get-FileHash -LiteralPath (Join-Path $installRoot $file.path) -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Installed bytes differ: $($file.path)"
    }
}
$shell = New-Object -ComObject WScript.Shell
foreach ($shortcut in @($desktopShortcut, (Join-Path $startFolder 'CoreVideo Pro.lnk'))) {
    $link = $shell.CreateShortcut($shortcut)
    if ($link.TargetPath -ne (Join-Path $installRoot 'StartCoreVideo.cmd') -or $link.WorkingDirectory -ne $installRoot) {
        throw 'Shortcut target/working directory mismatch.'
    }
}
$reportPath = Join-Path $work 'runtime-probe.json'
if ((Invoke-OwnedProcess (Join-Path $installRoot 'CoreVideoPro.WinUI.exe') @('--verify-runtime', ('"' + $reportPath + '"'))) -ne 0) { throw 'Installed runtime probe failed.' }
$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
if (-not $report.success -or $report.windowOpened) { throw 'Installed runtime is not self-contained or opened a window.' }
$uninstaller = Join-Path $work 'test-uninstaller.exe'
Copy-Item -LiteralPath (Join-Path $installRoot 'Uninstall.exe') -Destination $uninstaller
$marker = Join-Path $installRoot '.corevideo-alpha-install'
[IO.File]::WriteAllText($marker, 'incorrect-marker')
if ((Invoke-OwnedProcess $uninstaller @('/S', "_?=$installRoot")) -eq 0) { throw 'Incorrect install marker was accepted.' }
if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'CoreVideoPro.WinUI.exe'))) { throw 'Invalid-marker attempt deleted app files.' }
[IO.File]::WriteAllText($marker, $releaseId)
$secondRoot = Join-Path $work 'second-install'
if ((Invoke-OwnedProcess $installerPath @('/S', "/D=$secondRoot")) -ne 1638) { throw 'Duplicate version installation was not rejected.' }
if (Test-Path -LiteralPath $secondRoot) { throw 'Duplicate installation wrote a second app directory.' }
$sentinel = Join-Path $installRoot 'keep-user-recording.txt'
[IO.File]::WriteAllText($sentinel, 'preserve this user file')
if ((Invoke-OwnedProcess $uninstaller @('/S', "_?=$installRoot")) -ne 0) { throw 'Uninstall failed.' }
if ((Test-Path $registryPath) -or (Test-Path -LiteralPath $desktopShortcut) -or (Test-Path -LiteralPath $startFolder)) { throw 'Uninstall metadata or shortcuts remain.' }
foreach ($file in $manifest.files) {
    if (Test-Path -LiteralPath (Join-Path $installRoot $file.path)) { throw "Delivered file remained after uninstall: $($file.path)" }
}
if ((Get-Content -LiteralPath $sentinel -Raw) -ne 'preserve this user file') { throw 'Uninstall did not preserve the user file.' }
[ordered]@{ success=$true; releaseId=$releaseId; payloadFilesVerified=$manifest.files.Count
    silentInstall=$true; runtimeProbe=$true; shortcutTargets=$true; duplicateInstallRejected=$true
    invalidMarkerRejected=$true; silentUninstall=$true; userFilePreserved=$true; evidence=$work
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $work 'result.json')
