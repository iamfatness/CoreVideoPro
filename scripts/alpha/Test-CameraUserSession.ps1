$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
$Action = 'Install'; $AppDirectory = 'unchanged-app'; $OwnerSid = 'unchanged-owner'
. (Join-Path $PSScriptRoot 'CameraUserSession.ps1')
if ($Action -ne 'Install' -or $AppDirectory -ne 'unchanged-app' -or $OwnerSid -ne 'unchanged-owner') { throw 'Loading the helper changed installer arguments.' }
$count = 1
function Assert-True($Value, [string]$Message) { if (-not $Value) { throw $Message }; $script:count++ }
function Assert-Refused([scriptblock]$Call) { $refused=$false; try { & $Call | Out-Null } catch { $refused=$true }; Assert-True $refused 'Expected refusal.' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('CoreVideoUserAliasTest-' + [Guid]::NewGuid().ToString('N'))
$app = Join-Path $root 'app'; $runtimeRoot = Join-Path $root 'protected'
$appDll = Join-Path $app 'corevideo-virtualcam.dll'
New-Item -ItemType Directory -Path $app -Force | Out-Null
[IO.File]::WriteAllBytes($appDll, [byte[]]@(1,2,3,4))
$hash = Get-CameraPayloadSha256 $appDll
$version = Join-Path $runtimeRoot $hash
New-Item -ItemType Directory -Path $version -Force | Out-Null
$dll = Join-Path $version 'corevideo-virtualcam.dll'
Copy-Item -LiteralPath $appDll -Destination $dll
$machine = @{Path=$dll;Role='CoreVideoPro.VirtualCamera.v1';AppDirectory=$app;Sha256=$hash;ThreadingModel='Both'}
$old = @{Path='C:\Program Files\Common Files\CoreVideoProCamera\old\corevideo-virtualcam.dll';Role='CoreVideoPro.VirtualCamera.v1';AppDirectory='C:\Apps\previous';Sha256='old';ThreadingModel='Both'}
$callerView = $machine.Clone(); $desktopView = $old.Clone(); $sets=0; $running=$false; $corruptNextWrite=$false
# The regression is two different views for the same SID. Model only the
# independently dispatched worker here; real production keys/tasks are unused.
function Get-CameraMachineRegistration { param([switch]$CurrentUser) if($CurrentUser){$script:desktopView.Clone()}else{$script:machine.Clone()} }
function Set-CameraMachineRegistration { param($Registration,[switch]$CurrentUser)
    if(-not $CurrentUser){throw 'A user worker attempted a machine write.'}
    $script:sets++
    $script:desktopView=@{}
    foreach($field in @('Path','Role','AppDirectory','Sha256','ThreadingModel')){$script:desktopView[$field]=[string]$Registration.($field)}
    if($script:corruptNextWrite){$script:desktopView.Path='wrong-readback';$script:corruptNextWrite=$false}
}
function Test-OwnedLegacyCamera { param($Path) $false }
function Get-CameraRuntimeRoot { $script:runtimeRoot }
function Assert-CameraUserAppStopped { if($script:running){throw 'App running.'} }
$backup = Join-Path $app '.camera-desktop-registration-before.json'
try {
    $read = Invoke-CameraUserRegistration Inspect $app
    Assert-True (-not $read.matchesMachine) 'The desktop mismatch was hidden by the caller view.'
    Assert-True ($callerView.Path -eq $machine.Path -and $read.registration.Path -eq $old.Path) 'The wrong registry view qualified.'
    Assert-True (-not $read.cameraDeliveryVerified -and -not $read.servingIdentityVerified) 'Registration claimed camera delivery.'
    $null = Invoke-CameraUserRegistration Preflight $app
    Assert-True ($sets -eq 0) 'Preflight changed registration.'
    $fixed = Invoke-CameraUserRegistration Install $app
    Assert-True $fixed.matchesMachine 'Desktop alias did not migrate to the protected runtime.'
    Assert-True ((Get-Content $backup -Raw | ConvertFrom-Json).AppDirectory -eq $old.AppDirectory) 'The actual desktop backup was not retained.'
    $null = Invoke-CameraUserRegistration Install $app
    Assert-True ((Get-Content $backup -Raw | ConvertFrom-Json).AppDirectory -eq $old.AppDirectory) 'Repeated installation replaced the original backup.'
    $null = Invoke-CameraUserRegistration Rollback $app
    Assert-True ($desktopView.Path -eq $old.Path) 'Rollback did not restore the desktop alias.'
    $desktopView.Role='Foreign.Camera'; $beforeSets=$sets
    Assert-Refused { Invoke-CameraUserRegistration Preflight $app }
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    Assert-True ($sets -eq $beforeSets -and $desktopView.Role -eq 'Foreign.Camera') 'Foreign registration was overwritten.'
    $desktopView=$old.Clone(); $machine.AppDirectory='C:\Other\owner'
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    Assert-True ($desktopView.Path -eq $old.Path) 'Wrong machine owner changed the alias.'
    $machine.AppDirectory=$app
    [IO.File]::WriteAllBytes($dll,[byte[]]@(9))
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    Copy-Item -LiteralPath $appDll -Destination $dll -Force
    [IO.File]::WriteAllBytes($appDll,[byte[]]@(9))
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    Copy-Item -LiteralPath $dll -Destination $appDll -Force
    $running=$true; $beforeSets=$sets
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    $null=Invoke-CameraUserRegistration Inspect $app
    Assert-True ($sets -eq $beforeSets) 'Running-app refusal/inspection changed registration.'
    $running=$false; $corruptNextWrite=$true
    Assert-Refused { Invoke-CameraUserRegistration Install $app }
    Assert-True ($desktopView.Path -eq $old.Path) 'Readback failure did not restore the prior alias.'
    $null=Invoke-CameraUserRegistration Uninstall $app
    Assert-True ($desktopView.Path -eq $old.Path) 'Old uninstaller removed a different installation alias.'
    $null=Invoke-CameraUserRegistration Install $app
    $null=Invoke-CameraUserRegistration Uninstall $app
    Assert-True (-not $desktopView.Path) 'Own uninstaller left its alias.'
    foreach($file in @('CameraUserSession.ps1','Install-VirtualCamera.ps1')) {
        $errors=$null;$tokens=$null
        [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $file),[ref]$tokens,[ref]$errors)|Out-Null
        Assert-True ($errors.Count -eq 0) "Parser errors in $file"
    }
    Write-Output "$count desktop-user cases passed; no production registry, scheduled tasks or app processes changed."
} finally {
    foreach($file in @($backup,$appDll,$dll)){if([IO.File]::Exists($file)){[IO.File]::Delete($file)}}
    foreach($directory in @($app,$version,$runtimeRoot,$root)){if([IO.Directory]::Exists($directory)){[IO.Directory]::Delete($directory,$false)}}
}
