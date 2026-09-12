# Exercises a real silent install/uninstall in a new artifact directory. It never
# launches the product App or changes an existing installation or user settings.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$releaseId = [IO.Path]::GetFileNameWithoutExtension($installerPath).Replace('CoreVideoPro-Setup-', '')
if ($releaseId -notmatch '^(alpha|beta)-[0-9]{4}-[0-9]{2}-[0-9]{2}-[a-z0-9]+$') { throw 'Unexpected installer name.' }
$registryPath = "HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/CoreVideoPro-$releaseId"
$desktopShortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) "CoreVideo Pro $releaseId.lnk"
$startFolder = Join-Path ([Environment]::GetFolderPath('Programs')) "CoreVideo Pro $releaseId"
# T2.7 / #474: the ONE entry point that always opens the newest install.
$stableDesktop = Join-Path ([Environment]::GetFolderPath('Desktop')) 'CoreVideo Pro.lnk'
$stableStart = Join-Path ([Environment]::GetFolderPath('Programs')) 'CoreVideo Pro.lnk'
if ((Test-Path $registryPath) -or (Test-Path -LiteralPath $desktopShortcut) -or (Test-Path -LiteralPath $startFolder)) {
    throw 'This release already has installation metadata; refusing to alter it for testing.'
}
# The stable shortcuts are shared across releases, so a real install on this
# machine would own them. Refuse rather than clobber the operator's own.
$hadStableDesktop = Test-Path -LiteralPath $stableDesktop
$hadStableStart = Test-Path -LiteralPath $stableStart
if ($hadStableDesktop -or $hadStableStart) {
    throw 'A CoreVideo Pro shortcut already exists; refusing to alter a real installation for testing.'
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
# T2.7: the version-named shortcut lives in the Start menu ONLY. A desktop full
# of version-named shortcuts is how the owner launched a three-day-old build and
# filed a defect against code that already had the fix.
if (Test-Path -LiteralPath $desktopShortcut) { throw 'Install created a version-named desktop shortcut.' }
foreach ($shortcut in @($stableDesktop, $stableStart, (Join-Path $startFolder 'CoreVideo Pro.lnk'))) {
    if (-not (Test-Path -LiteralPath $shortcut)) { throw "Expected shortcut missing: $shortcut" }
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
$marker = Join-Path $installRoot '.corevideo-prerelease-install'
[IO.File]::WriteAllText($marker, 'incorrect-marker')
if ((Invoke-OwnedProcess $uninstaller @('/S', "_?=$installRoot")) -eq 0) { throw 'Incorrect install marker was accepted.' }
if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'CoreVideoPro.WinUI.exe'))) { throw 'Invalid-marker attempt deleted app files.' }
[IO.File]::WriteAllText($marker, $releaseId)
$secondRoot = Join-Path $work 'second-install'
if ((Invoke-OwnedProcess $installerPath @('/S', "/D=$secondRoot")) -ne 1638) { throw 'Duplicate version installation was not rejected.' }
if (Test-Path -LiteralPath $secondRoot) { throw 'Duplicate installation wrote a second app directory.' }
$sentinel = Join-Path $installRoot 'keep-user-recording.txt'
[IO.File]::WriteAllText($sentinel, 'preserve this user file')
# T2.7 / #474 item 2. Stand in for what first launch downloads: ~195 MB of
# FFmpeg runtime that lived in NO uninstall list, so RMDir (non-recursive)
# silently failed and the whole thing stayed behind. Two uninstalled versions on
# the owner's machine were still holding 10 files and 195 MB each on 2026-09-12.
# Shaped exactly like Install-MediaRuntime.ps1 writes it, manifest included.
$runtimeFiles = @('ffmpeg.exe', 'avcodec-62.dll', 'swscale-9.dll', 'notices\ffmpeg\LICENSE.txt')
New-Item -ItemType Directory -Force (Join-Path $installRoot 'notices\ffmpeg') | Out-Null
foreach ($relative in $runtimeFiles) { [IO.File]::WriteAllText((Join-Path $installRoot $relative), 'runtime') }
[IO.File]::WriteAllText((Join-Path $installRoot 'notices\ffmpeg\download-provenance.txt'), 'provenance')
$runtimeManifest = Join-Path $installRoot 'notices\ffmpeg\installed-files.txt'
# Written exactly the way Install-MediaRuntime.ps1 writes it: UTF-8 with NO BOM.
# That is the bug this test caught on 2026-09-12 - Set-Content -Encoding UTF8
# emits a BOM under Windows PowerShell 5.1, NSIS reads it as three literal
# characters, and the first path in the manifest then deletes nothing at all.
$manifestLines = [string[]]($runtimeFiles + @('notices\ffmpeg\download-provenance.txt', 'notices\ffmpeg\installed-files.txt'))
# A manifest is a file on disk, and a file that decides what an uninstaller
# deletes is worth hardening. These must be refused, not followed.
$manifestLines += @('..\escape-attempt.txt', 'C:\Windows\System32\absolute-attempt.txt', '\rooted-attempt.txt')
[IO.File]::WriteAllLines($runtimeManifest, $manifestLines, (New-Object Text.UTF8Encoding $false))
$escapeTarget = Join-Path (Split-Path -Parent $installRoot) 'escape-attempt.txt'
[IO.File]::WriteAllText($escapeTarget, 'must survive')
if ((Invoke-OwnedProcess $uninstaller @('/S', "_?=$installRoot")) -ne 0) { throw 'Uninstall failed.' }
foreach ($relative in $runtimeFiles) {
    if (Test-Path -LiteralPath (Join-Path $installRoot $relative)) {
        throw "First-run media runtime remained after uninstall: $relative"
    }
}
if (Test-Path -LiteralPath $runtimeManifest) { throw 'Media runtime manifest remained after uninstall.' }
if (-not (Test-Path -LiteralPath $escapeTarget)) { throw 'Uninstall followed a ".." path out of the install directory.' }
if ((Test-Path $registryPath) -or (Test-Path -LiteralPath $desktopShortcut) -or (Test-Path -LiteralPath $startFolder)) { throw 'Uninstall metadata or shortcuts remain.' }
if ((Test-Path -LiteralPath $stableDesktop) -or (Test-Path -LiteralPath $stableStart)) {
    throw 'Uninstall left the stable shortcut pointing at a removed install.'
}
foreach ($file in $manifest.files) {
    if (Test-Path -LiteralPath (Join-Path $installRoot $file.path)) { throw "Delivered file remained after uninstall: $($file.path)" }
}
if ((Get-Content -LiteralPath $sentinel -Raw) -ne 'preserve this user file') { throw 'Uninstall did not preserve the user file.' }
[ordered]@{ success=$true; releaseId=$releaseId; payloadFilesVerified=$manifest.files.Count
    silentInstall=$true; runtimeProbe=$true; shortcutTargets=$true; duplicateInstallRejected=$true
    invalidMarkerRejected=$true; silentUninstall=$true; userFilePreserved=$true
    stableShortcutOnly=$true; mediaRuntimeRemoved=$true; manifestEscapeRefused=$true; evidence=$work
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $work 'result.json')
