# Explicitly install/remove the machine-readable camera runtime for one app.
# Never infer an old development checkout from whatever binaries happen to exist.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$AppDirectory, [switch]$Unregister)
$ErrorActionPreference = 'Stop'
$action = if ($Unregister) { 'Uninstall' } else { 'Install' }
& (Join-Path $PSScriptRoot 'alpha/Install-VirtualCamera.ps1') -Action $action -AppDirectory $AppDirectory -AllowElevation
