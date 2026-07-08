# Registers (or unregisters) corevideo-virtualcam.dll so the Windows Frame Server
# can load CoreVideo Pro's virtual-camera media source (docs/virtual-camera-spec.md
# V2b). This is a USER-MODE COM DLL - no kernel driver, no signing. It does need
# an elevated shell because DllRegisterServer writes the InprocServer32 CLSID key
# under HKEY_CLASSES_ROOT (machine-wide). Run from an Administrator PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File scripts\register-virtualcam.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\register-virtualcam.ps1 -Unregister
#
# After registering, enable the virtual camera in the app; "CoreVideo Pro Camera"
# should then appear in the Windows Camera app and in Zoom/Teams/Meet's camera list.

param(
  [switch]$Unregister
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

# Prefer the dev build output; fall back to the packaged publish dir.
$candidates = @(
  (Join-Path $repoRoot "native\build-dev\corevideo-virtualcam.dll"),
  (Join-Path $repoRoot "native\build-dev\Release\corevideo-virtualcam.dll")
)
$dll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) {
  Write-Error "corevideo-virtualcam.dll not found. Build it first: npm run build:native-dev (it builds with COREVIDEO_WITH_VIRTUALCAM=ON)."
  exit 1
}

# Elevation check.
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
  Write-Error "This must run from an elevated (Administrator) PowerShell - DllRegisterServer writes an HKCR CLSID key."
  exit 1
}

$action = if ($Unregister) { "Unregistering" } else { "Registering" }
Write-Host "$action $dll ..."
$args = if ($Unregister) { @("/s", "/u", $dll) } else { @("/s", $dll) }
$proc = Start-Process -FilePath "regsvr32.exe" -ArgumentList $args -Wait -PassThru
if ($proc.ExitCode -ne 0) {
  Write-Error "regsvr32 failed with exit code $($proc.ExitCode)."
  exit $proc.ExitCode
}
if ($Unregister) {
  Write-Host "Unregistered. 'CoreVideo Pro Camera' will no longer be available."
} else {
  Write-Host "Registered. Enable the virtual camera in the app, then pick 'CoreVideo Pro Camera' in Zoom/Teams/Meet."
}
