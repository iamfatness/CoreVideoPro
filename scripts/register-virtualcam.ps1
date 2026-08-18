# Registers (or unregisters) corevideo-virtualcam.dll so the Windows Frame Server
# can load CoreVideo Pro's virtual-camera media source (docs/virtual-camera-spec.md
# V2b). USER-MODE COM DLL, PER-USER registration (HKCU\Software\Classes) - NO admin
# / elevation required. Run from a normal PowerShell:
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

# No elevation needed - DllRegisterServer writes a per-user HKCU CLSID key.
$action = if ($Unregister) { "Unregistering" } else { "Registering" }
Write-Host "$action $dll ..."
$quotedDll = '"' + $dll + '"'
$registerArgs = if ($Unregister) { @("/s", "/u", $quotedDll) } else { @("/s", $quotedDll) }
$proc = Start-Process -FilePath "regsvr32.exe" -ArgumentList $registerArgs -Wait -PassThru
if ($proc.ExitCode -ne 0) {
  Write-Error "regsvr32 failed with exit code $($proc.ExitCode)."
  exit $proc.ExitCode
}
if ($Unregister) {
  Write-Host "Unregistered. 'CoreVideo Pro Camera' will no longer be available."
} else {
  Write-Host "Registered. Enable the virtual camera in the app, then pick 'CoreVideo Pro Camera' in Zoom/Teams/Meet."
}
