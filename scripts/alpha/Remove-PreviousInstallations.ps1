[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$InspectOnly,
    # Test hooks (#883): the uninstall registry root, Start-menu folder and camera owner
    # key are overridable so Test-CleanupEnvironment.ps1 can run this exact script in a
    # child host against throwaway fixtures. Setup never passes them.
    [string]$RegistryRoot = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall',
    [string]$ProgramsDirectory = ([Environment]::GetFolderPath('Programs')),
    [string]$CameraRegistrationKey = '',
    # Names the log: %LOCALAPPDATA%\CoreVideoPro\installer-logs\<release>-retire.log.
    [string]$ReleaseId = '',
    # Extra commands to prove resolvable before anything runs (test hook for the failure path).
    [string[]]$RequiredCommands = @(),
    # Manual runs only: keep operator content in place instead of moving it (setup never passes it).
    [switch]$LeaveUnknown)
$ErrorActionPreference = 'Stop'
if (!$ReleaseId) { $ReleaseId = [IO.Path]::GetFileName($AppDirectory.TrimEnd([char]'\', [char]'/')) }
$logPath = [IO.Path]::Combine($env:LOCALAPPDATA, 'CoreVideoPro', 'installer-logs', "$ReleaseId-retire.log")
# .NET only: this must work even when command resolution is what is broken (#883).
function Write-RetireLog([string]$Message) {
    try {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($logPath))
        [IO.File]::AppendAllText($logPath, ('{0:o} {1}{2}' -f [DateTime]::Now, $Message, [Environment]::NewLine))
    } catch { [Console]::Error.WriteLine("Cannot write cleanup log ${logPath}: $($_.Exception.Message)") }
}
Import-Module (Join-Path $PSScriptRoot 'PreviousInstallations.psm1') -Force
try {
$environment = Initialize-SetupHelperEnvironment -RequiredCommands $RequiredCommands -ScriptPath $PSCommandPath
Write-RetireLog "environment (PSModulePath reset for this process only, owner ruling D5): $(Format-SetupHelperDiagnostics $environment)"
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$app = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $AppDirectory).Path).TrimEnd('\')
$root = Join-Path $env:LOCALAPPDATA 'Programs/CoreVideoPro'
# Custom QA destinations must never retire the operator's real installations.
if ((Split-Path -Parent $app) -ne $root) { Write-RetireLog 'skipped: custom install destination'; Write-Output 'Previous installation cleanup skipped: custom install destination.'; exit 0 }
if ($CameraRegistrationKey) {
    # Only the dedicated test namespace may stand in for the machine camera owner.
    if ($CameraRegistrationKey -notmatch '^Software\\CoreVideoProInstallerTests\\[0-9a-f]{32}\\') { throw "Camera test key outside the test namespace: $CameraRegistrationKey" }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey($CameraRegistrationKey)
        if (!$key) { throw "Camera test key missing: $CameraRegistrationKey" }
        try { $camera = [ordered]@{ Role=[string]$key.GetValue('CoreVideoOwnerRole'); AppDirectory=[string]$key.GetValue('CoreVideoOwnerAppDirectory') } } finally { $key.Dispose() }
    } finally { $base.Dispose() }
} else { $camera = Get-CameraMachineRegistration }
if ($camera.Role -ne 'CoreVideoPro.VirtualCamera.v1' -or $camera.AppDirectory -ne $app) {
    throw 'The new version must own webcam registration before retiring previous versions.'
}
# Fail closed when process inventory is unavailable; never terminate old sessions.
$processes = @(Get-CimInstance Win32_Process -ErrorAction Stop)
if (@($processes | Where-Object { $_.Name -match '^(CoreVideoPro|corevideo)' -and !$_.ExecutablePath }).Count) {
    throw 'Cannot establish executable paths for running CoreVideo processes.'
}
$running = @($processes.ExecutablePath)
# Pass 1: registered versions (uninstall entry + marker + manifest), as before.
$plans = @(Get-PreviousInstallationPlan -CurrentDirectory $app -InstallRoot $root -RegistryRoot $RegistryRoot -RunningPaths $running)
if (!$InspectOnly) {
    $plans = @($plans | ForEach-Object { Remove-PreviousInstallation -Plan $_ -ProgramsDirectory $ProgramsDirectory })
}
# Pass 2 (#883): residual version folders with no registration, including any pass 1 just
# unregistered but could not empty. Never touch a folder a camera registration points into.
$protected = @($camera.AppDirectory, $camera.Path)
if (!$CameraRegistrationKey) { $desktop = Get-CameraMachineRegistration -CurrentUser; $protected += @($desktop.AppDirectory, $desktop.Path) }
$residual = @(Get-ResidualInstallationPlan -CurrentDirectory $app -InstallRoot $root -RegistryRoot $RegistryRoot -RunningPaths $running -ProtectedPaths @($protected | Where-Object { $_ }))
if ($InspectOnly) {
    [ordered]@{ registered = $plans; residual = $residual } | ConvertTo-Json -Depth 6
    exit 0
}
# Owner ruling D4: operator content is moved to a dated backup folder, never deleted.
$backupFolder = New-ResidualBackupFolder -BackupRoot ([IO.Path]::Combine($env:LOCALAPPDATA, 'CoreVideoPro', 'backups'))
$residual = @($residual | ForEach-Object { Remove-ResidualInstallation -Plan $_ -BackupFolder $backupFolder -LeaveUnknown:$LeaveUnknown })
$errors = @($plans | Where-Object status -eq 'retained') + @($residual | Where-Object { $_.status -eq 'skipped' -or ($_.status -eq 'retained' -and $_.reason -notlike 'operator content:*') })
$backedUp = @($residual | Where-Object status -eq 'backed-up')
# Ruling D4: report the backup wherever files were moved, including a folder that moved some
# files and was then retained (e.g. a locked file), so the warning box names it too.
$moved = @($residual | Where-Object { $_.moved -gt 0 })
$summary = [ordered]@{
    registeredRetired = @($plans | Where-Object status -eq 'removed').Count
    residualRemoved = @($residual | Where-Object status -eq 'removed').Count
    residualBackedUp = $backedUp.Count
    retained = @($plans | Where-Object status -eq 'retained').Count + @($residual | Where-Object { $_.status -in 'retained','skipped' }).Count
    backupFolder = if ($moved.Count) { $backupFolder } else { '' }
    filesBackedUp = [int](($moved | ForEach-Object { $_.moved } | Measure-Object -Sum).Sum)
}
$report = [ordered]@{
    summary = $summary
    registered = @($plans | Select-Object releaseId,directory,status,reason,preserved)
    residual = @($residual | Select-Object releaseId,directory,status,reason,backedUpTo,moved)
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $app 'previous-installations-cleanup.json'), $report, (New-Object Text.UTF8Encoding $false))
# A copy outside the version folder survives the next upgrade's cleanup.
try { [IO.File]::WriteAllText([IO.Path]::ChangeExtension($logPath, '.json'), $report, (New-Object Text.UTF8Encoding $false)) }
catch { [Console]::Error.WriteLine("Cannot copy the cleanup report to installer-logs: $($_.Exception.Message)") }
foreach ($item in @($plans) + @($residual)) { Write-RetireLog "$($item.releaseId): $($item.status) $($item.reason)" }
# The backup line comes FIRST: NSIS keeps only the head of stdout (NSIS_MAX_STRLEN 1024).
if ($moved.Count) {
    $line = "Operator files from $($moved.Count) old version folder(s) ($($summary.filesBackedUp) files) were MOVED, not deleted, to: $backupFolder"
    Write-RetireLog $line; Write-Output $line
}
foreach ($item in $errors) { Write-Output "Retained $($item.releaseId): $($item.reason)" }
foreach ($item in @($residual | Where-Object { $_.status -eq 'retained' -and $_.reason -like 'operator content:*' })) { Write-Output "Left in place $($item.releaseId): $($item.reason)" }
$retired = $summary.registeredRetired + $summary.residualRemoved
$last = "Previous installation cleanup: $retired retired, $($summary.residualBackedUp) backed up, $($summary.retained) retained."
Write-RetireLog $last
Write-Output $last
# 0 = clean; 2 = clean, operator files moved to the backup folder (setup says where); 1 = something retained.
if ($errors.Count) { exit 1 }
if ($backedUp.Count) { exit 2 }
exit 0
} catch {
    Write-RetireLog "FAILED: $($_.Exception.Message)"
    throw
}
