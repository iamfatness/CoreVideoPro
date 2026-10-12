[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$InspectOnly)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PreviousInstallations.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$app = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $AppDirectory).Path).TrimEnd('\')
$root = Join-Path $env:LOCALAPPDATA 'Programs/CoreVideoPro'
# Custom QA destinations must never retire the operator's real installations.
if ((Split-Path -Parent $app) -ne $root) { Write-Output 'Previous installation cleanup skipped: custom install destination.'; exit 0 }
$camera = Get-CameraMachineRegistration
if ($camera.Role -ne 'CoreVideoPro.VirtualCamera.v1' -or $camera.AppDirectory -ne $app) {
    throw 'The new version must own webcam registration before retiring previous versions.'
}
# Fail closed when process inventory is unavailable; never terminate old sessions.
$processes = @(Get-CimInstance Win32_Process -ErrorAction Stop)
if (@($processes | Where-Object { $_.Name -match '^(CoreVideoPro|corevideo)' -and !$_.ExecutablePath }).Count) {
    throw 'Cannot establish executable paths for running CoreVideo processes.'
}
$plans = @(Get-PreviousInstallationPlan -CurrentDirectory $app -InstallRoot $root -RegistryRoot 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall' -RunningPaths @($processes.ExecutablePath))
if (!$InspectOnly) {
    $plans = @($plans | ForEach-Object { Remove-PreviousInstallation -Plan $_ -ProgramsDirectory ([Environment]::GetFolderPath('Programs')) })
}
$plans | ConvertTo-Json -Depth 5
if (!$InspectOnly) {
    $plans | Select-Object releaseId,status,reason,preserved | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $app 'previous-installations-cleanup.json')
    if (@($plans | Where-Object status -eq 'retained').Count) { exit 1 }
}
