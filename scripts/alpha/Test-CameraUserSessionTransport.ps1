# Read-only OS transport smoke test. No installation, camera activation, app
# launch, user-alias write or live media is performed by Inspect.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'CameraUserSession.ps1')
$app = [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).TrimEnd('\')
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$result = Invoke-CameraUserSession -Action Inspect -AppDirectory $app -OwnerSid $sid
if (-not $result.ok -or $result.workerPid -eq $PID -or $result.workerPid -le 0 -or
    $result.boundary -ne 'independent-interactive-user' -or $result.ownerSid -ne $sid -or
    $result.userSession.cameraDeliveryVerified -or $result.userSession.servingIdentityVerified) { throw 'Independent read-only transport did not produce valid scoped evidence.' }
Write-Output 'Independent desktop-user transport passed; camera delivery and installed Unassign remain unverified.'
