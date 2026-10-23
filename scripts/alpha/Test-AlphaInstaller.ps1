# Exercises a real silent install/uninstall in a new artifact directory. It never
# launches the product App or changes an existing installation or user settings.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer,
      [string]$CameraModulePath = (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1'),
      # Short user-owned working root (#863/#888): the install directory must not inherit a long checkout path.
      [string]$WorkRoot = (Join-Path (Join-Path $env:LOCALAPPDATA 'Temp\cvp-qa') ([Guid]::NewGuid().ToString('N').Substring(0, 8))),
      [int]$MaxWorkPathLength = 120)
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
# T2.1 / #433 baseline: never alter a LocalDumps configuration this test did not write.
# Read through the 64-bit registry view: the installer is 32-bit, but WerFault reads the 64-bit view, and
# the HKLM: PSDrive would follow THIS process's view and could show the wrong hive.
function Get-DumpKey64([string]$exe) {
    $b = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $k = $b.OpenSubKey("SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\$exe")
        if (-not $k) { return $null }
        try {
            [pscustomobject]@{ Managed = ($null -ne $k.GetValue('CoreVideoProInstaller')); DumpType = $k.GetValue('DumpType'); DumpCount = $k.GetValue('DumpCount')
                DumpFolder = $k.GetValue('DumpFolder', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                DumpFolderKind = $k.GetValueKind('DumpFolder').ToString() }
        } finally { $k.Dispose() }
    } finally { $b.Dispose() }
}
# Same retention rule as Install-CrashDumps.ps1: keys stay while another registered version still exists.
function Test-OtherCvpInstallRegistered {
    foreach ($k in Get-ChildItem 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall' -ErrorAction SilentlyContinue) {
        if ($k.PSChildName -notlike 'CoreVideoPro-*' -or $k.PSChildName -eq "CoreVideoPro-$releaseId") { continue }
        $loc = (Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue).InstallLocation
        if ($loc -and (Test-Path -LiteralPath $loc)) { return $true }
    }
    return $false
}
$dumpExecutables = 'CoreVideoPro.WinUI.exe', 'corevideo-native.exe', 'corevideo-zoom-engine.exe'
$isAdmin = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$priorDumps = @{}
foreach ($exe in $dumpExecutables) { $priorDumps[$exe] = Get-DumpKey64 $exe }
$videosFolder = [Environment]::GetFolderPath('MyVideos')
$recordingFolderExisted = Test-Path -LiteralPath $(if ($videosFolder) { Join-Path $videosFolder 'CoreVideo Pro' } else { Join-Path $env:LOCALAPPDATA 'CoreVideo Pro' })
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$work = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkRoot)
if ($work.Length -gt $MaxWorkPathLength) { throw "Working root is $($work.Length) characters (limit $MaxWorkPathLength); use a shorter -WorkRoot: $work" }
if ((Test-Path -LiteralPath $work) -and @(Get-ChildItem -LiteralPath $work -Force).Count -gt 0) { throw "Working root is not empty; refusing to reuse it: $work" }
New-Item -ItemType Directory -Path $work -Force | Out-Null
$evidenceCopy = Join-Path $repoRoot ('artifacts/installer-validation/' + (Split-Path -Leaf $work))
$installRoot = Join-Path $work 'installed app'
$succeeded = $false
try {
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
Import-Module $CameraModulePath -Force
$priorCamera = Get-CameraMachineRegistration
$priorUserCamera = Get-CameraMachineRegistration -CurrentUser
try {
    # Force a pre-commit failure without modifying machine camera ownership.
    # Setup must release its cwd and remove the empty root so retry can work.
    Set-CameraMachineRegistration @{Path=(Join-Path $work 'unknown-camera.dll');Role='InstallerQA.UnknownOwner';AppDirectory='';Sha256='';ThreadingModel='Both'} -CurrentUser
    if ((Invoke-OwnedProcess $installerPath @('/S', "/D=$installRoot")) -ne 1603) { throw 'Unknown camera owner was not rejected.' }
    if ((Test-Path -LiteralPath $installRoot) -or (Test-Path $registryPath)) { throw 'Failed setup left its destination or registration; retry would fail.' }
    $afterFailure = Get-CameraMachineRegistration
    foreach ($field in @('Path','Role','AppDirectory','Sha256')) {
        if ($afterFailure[$field] -ne $priorCamera[$field]) { throw "Failed setup changed camera ownership: $field" }
    }
} finally { Set-CameraMachineRegistration $priorUserCamera -CurrentUser }
if ((Invoke-OwnedProcess $installerPath @('/S', "/D=$installRoot")) -ne 0) { throw "Silent installation failed. Evidence: $work" }
# T2.1 / #433: recording folder + WER LocalDumps. The recording folder must be the app's own default
# (RecordingFolderPolicy: Videos\CoreVideo Pro, else %LOCALAPPDATA%\CoreVideo Pro) and survive uninstall.
$recordingFolder = if ($videosFolder) { Join-Path $videosFolder 'CoreVideo Pro' } else { Join-Path $env:LOCALAPPDATA 'CoreVideo Pro' }
if (-not (Test-Path -LiteralPath $recordingFolder -PathType Container)) { throw "Install did not create the recording folder: $recordingFolder" }
$crashLog = Join-Path $env:LOCALAPPDATA "CoreVideoPro\installer-logs\$releaseId-crashdumps.log"
$crashDumpsExpected = $null
if ($isAdmin) {
    # Silent setup is elevated here, so the HKLM write must have happened (or an existing foreign config was preserved).
    foreach ($exe in $dumpExecutables) {
        $item = Get-DumpKey64 $exe
        if (-not $item) { throw "LocalDumps key missing from the 64-bit registry view after elevated install: $exe" }
        if ($priorDumps[$exe] -and -not $priorDumps[$exe].Managed) { continue }   # foreign config: must be untouched (checked after uninstall)
        if ($item.DumpType -ne 2 -or $item.DumpCount -ne 5 -or $item.DumpFolder -ne '%LOCALAPPDATA%\CoreVideoPro\crashes' -or
            $item.DumpFolderKind -ne 'ExpandString') { throw "LocalDumps values wrong for $exe" }
    }
    $crashDumpsExpected = 'configured'
} else {
    # Not elevated: setup must still succeed (it did, exit 0) and must say loudly that dumps were not enabled.
    if (-not (Test-Path -LiteralPath $crashLog) -or (Get-Content -LiteralPath $crashLog -Raw) -notmatch 'NOT configured') {
        throw 'Unelevated install did not log that crash dumps were not configured.'
    }
    $crashDumpsExpected = 'refused-loudly'
}
$inspectHost = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$inspectionJson = & $inspectHost -NoProfile -ExecutionPolicy Bypass -File (Join-Path $installRoot 'Install-VirtualCamera.ps1') -Action Inspect -AppDirectory $installRoot
if ($LASTEXITCODE -ne 0) { throw 'Installed desktop-user camera inspection failed.' }
$inspection = $inspectionJson | ConvertFrom-Json
$inspection | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $work 'desktop-user-camera-registration.json')
if (-not $inspection.desktopUser.ok -or $inspection.desktopUser.boundary -ne 'independent-interactive-user' -or
    -not $inspection.desktopUser.userSession.matchesMachine -or -not $inspection.registeredFileMatchesManifest) {
    throw 'The independently observed desktop-user camera alias does not match the installed machine runtime.'
}
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
if (Test-Path -LiteralPath (Join-Path $installRoot '.camera-desktop-registration-before.json')) { throw 'Uninstall left its desktop camera backup.' }
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
if (-not (Test-Path -LiteralPath $recordingFolder)) { throw 'Uninstall removed the recording folder.' }
$keepForOtherInstall = Test-OtherCvpInstallRegistered
foreach ($exe in $dumpExecutables) {
    # Keys this installer wrote are gone (unless another registered version still needs them);
    # a configuration that pre-existed and was not ours is exactly as it was.
    $after = Get-DumpKey64 $exe
    if ($after) {
        if ($isAdmin -and -not $keepForOtherInstall -and $after.Managed) { throw "Uninstall left the LocalDumps key for $exe." }
    } elseif ($priorDumps[$exe] -and ($keepForOtherInstall -or -not $priorDumps[$exe].Managed)) { throw "Uninstall removed a LocalDumps key it must keep: $exe" }
}
if (-not $recordingFolderExisted -and @(Get-ChildItem -LiteralPath $recordingFolder -Force).Count -eq 0) { Remove-Item -LiteralPath $recordingFolder }
if ((Get-Content -LiteralPath $sentinel -Raw) -ne 'preserve this user file') { throw 'Uninstall did not preserve the user file.' }
[ordered]@{ success=$true; releaseId=$releaseId; payloadFilesVerified=$manifest.files.Count
    silentInstall=$true; runtimeProbe=$true; shortcutTargets=$true; duplicateInstallRejected=$true
    invalidMarkerRejected=$true; silentUninstall=$true; userFilePreserved=$true
    stableShortcutOnly=$true; mediaRuntimeRemoved=$true; manifestEscapeRefused=$true; evidence=$evidenceCopy; workDirectory=$work
    failedInstallRetry=$true; failedInstallCameraPreserved=$true; independentDesktopRegistration=$true
    recordingFolderCreated=$true; crashDumps=$crashDumpsExpected
    desktopShortcutCameraDelivery='unverified'; desktopShortcutUnassign='unverified'
} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $work 'result.json')
$succeeded = $true
} finally {
    # Keep the evidence where it used to live (the CI workflow uploads artifacts/installer-validation
    # with if: always()), whether the run passed or failed.
    New-Item -ItemType Directory -Path $evidenceCopy -Force | Out-Null
    Get-ChildItem -LiteralPath $work -Filter *.json -ErrorAction SilentlyContinue | Copy-Item -Destination $evidenceCopy -Force
    foreach ($extra in @('test-uninstaller.exe', 'escape-attempt.txt')) {
        if (Test-Path -LiteralPath (Join-Path $work $extra)) { Copy-Item -LiteralPath (Join-Path $work $extra) -Destination $evidenceCopy -Force }
    }
    if (-not $succeeded) { Write-Warning "Installer test failed. Working folder: $work; evidence copy: $evidenceCopy" }
}
