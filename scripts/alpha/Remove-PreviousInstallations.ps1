[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$InspectOnly,
    # Test hooks (#883): the uninstall registry root, Start-menu folder and camera owner
    # key are overridable so Test-CleanupEnvironment.ps1 can run this exact script in a
    # child host against throwaway fixtures. Setup never passes them.
    [string]$RegistryRoot = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall',
    [string]$ProgramsDirectory = ([Environment]::GetFolderPath('Programs')),
    [string]$CameraRegistrationKey = '')
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PreviousInstallations.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$app = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $AppDirectory).Path).TrimEnd('\')
$root = Join-Path $env:LOCALAPPDATA 'Programs/CoreVideoPro'
# Custom QA destinations must never retire the operator's real installations.
if ((Split-Path -Parent $app) -ne $root) { Write-Output 'Previous installation cleanup skipped: custom install destination.'; exit 0 }
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
