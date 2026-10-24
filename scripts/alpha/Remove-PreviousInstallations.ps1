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
    [string[]]$RequiredCommands = @())
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
$plans = @(Get-PreviousInstallationPlan -CurrentDirectory $app -InstallRoot $root -RegistryRoot $RegistryRoot -RunningPaths @($processes.ExecutablePath))
if (!$InspectOnly) {
    $plans = @($plans | ForEach-Object { Remove-PreviousInstallation -Plan $_ -ProgramsDirectory $ProgramsDirectory })
}
$plans | ConvertTo-Json -Depth 5
if (!$InspectOnly) {
    $plans | Select-Object releaseId,status,reason,preserved | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $app 'previous-installations-cleanup.json')
    if (@($plans | Where-Object status -eq 'retained').Count) { exit 1 }
}
} catch {
    Write-RetireLog "FAILED: $($_.Exception.Message)"
    throw
}
