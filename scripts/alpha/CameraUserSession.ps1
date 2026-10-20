[CmdletBinding()]
param([switch]$Worker,
    [ValidateSet('Preflight','Inspect','Install','Uninstall','Rollback')][string]$WorkerAction = 'Inspect',
    [string]$WorkerAppDirectory, [string]$WorkerOwnerSid, [string]$WorkerPipeName, [string]$WorkerRequestId)

# A child of an installer/QA process can inherit a private registry view even
# without package identity. Only an independent interactive-user worker owns
# this part of installation. Never use its caller's HKCU as desktop evidence.
function Invoke-CameraUserRegistration {
    param([string]$Action, [string]$AppDirectory)
    $before = Get-CameraMachineRegistration -CurrentUser
    $machine = Get-CameraMachineRegistration
    $backupPath = Join-Path $AppDirectory '.camera-desktop-registration-before.json'
    if ($Action -eq 'Preflight' -or $Action -eq 'Install') {
        $decision = Get-CameraOwnershipDecision -Action Install -RegisteredPath $before.Path -OwnerRole $before.Role -OwnerAppDirectory $before.AppDirectory -AppDirectory $AppDirectory -OwnedLegacy (Test-OwnedLegacyCamera $before.Path)
        if ($decision -eq 'Conflict') { throw 'The desktop user has a camera registration with unknown ownership; it was preserved.' }
    }
    if ($Action -in @('Install','Uninstall','Rollback')) {
        Assert-CameraUserAppStopped
        if ($Action -eq 'Install') {
            $expectedPath = Join-Path (Join-Path (Get-CameraRuntimeRoot) $machine.Sha256) 'corevideo-virtualcam.dll'
            if ($machine.Role -ne 'CoreVideoPro.VirtualCamera.v1' -or $machine.AppDirectory -ne $AppDirectory -or
                $machine.Sha256 -notmatch '^[a-f0-9]{64}$' -or $machine.Path -ne $expectedPath -or
                (Get-CameraPayloadSha256 $machine.Path) -ne $machine.Sha256 -or
                (Get-CameraPayloadSha256 (Join-Path $AppDirectory 'corevideo-virtualcam.dll')) -ne $machine.Sha256) {
                throw 'Machine camera ownership or installed/protected DLL identity differs; desktop alias unchanged.'
            }
            if ($before.AppDirectory -ne $AppDirectory) {
                $before | ConvertTo-Json | Set-Content -LiteralPath $backupPath
            }
            try {
                Set-CameraMachineRegistration $machine -CurrentUser
                $after = Get-CameraMachineRegistration -CurrentUser
                foreach ($field in @('Path','Role','AppDirectory','Sha256','ThreadingModel')) {
                    if ($after[$field] -ne $machine[$field]) { throw "Desktop camera registration readback differs: $field" }
                }
            } catch { Set-CameraMachineRegistration $before -CurrentUser; throw }
        } elseif ($before.Role -eq 'CoreVideoPro.VirtualCamera.v1' -and $before.AppDirectory -eq $AppDirectory) {
            if ($Action -eq 'Rollback') {
                $saved = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
                Set-CameraMachineRegistration $saved -CurrentUser
            } else { Set-CameraMachineRegistration @{Path=''} -CurrentUser }
        }
    }
    $after = Get-CameraMachineRegistration -CurrentUser
    [ordered]@{ before=$before; registration=$after; machineRegistration=$machine;
        matchesMachine=($after.Path -eq $machine.Path -and $after.Role -eq $machine.Role -and $after.AppDirectory -eq $machine.AppDirectory -and $after.Sha256 -eq $machine.Sha256);
        servingIdentityVerified=$false; cameraDeliveryVerified=$false }
}

function Assert-CameraUserAppStopped {
    if (Get-Process -Name 'CoreVideoPro.WinUI','corevideo-native' -ErrorAction SilentlyContinue) {
        throw 'Close CoreVideo Pro before changing desktop camera registration.'
    }
}

function Invoke-CameraUserSession {
    param([ValidateSet('Preflight','Inspect','Install','Uninstall','Rollback')][string]$Action,
        [Parameter(Mandatory=$true)][string]$AppDirectory,
        [Parameter(Mandatory=$true)][ValidatePattern('^S-1-5-21-[0-9]+-[0-9]+-[0-9]+-[0-9]+$')][string]$OwnerSid)
    $id = [Guid]::NewGuid().ToString('N')
    $name = 'CoreVideo-CameraUser-' + $id
    $workerPath = Join-Path $PSScriptRoot 'CameraUserSession.ps1'
    # A pipe avoids trusting another potentially private AppData file view for
    # the reply and leaves no generated files to strand a failed installation.
    $service = New-Object -ComObject 'Schedule.Service'
    $service.Connect()
    $folder = $service.GetFolder('\')
    $definition = $service.NewTask(0)
    $definition.Principal.UserId = $OwnerSid
    $definition.Principal.LogonType = 3 # TASK_LOGON_INTERACTIVE_TOKEN
    $definition.Principal.RunLevel = 0 # Limited, including from elevated setup.
    $definition.Settings.Hidden = $true
    $definition.Settings.ExecutionTimeLimit = 'PT1M'
    $definition.Settings.DisallowStartIfOnBatteries = $false
    $definition.Settings.StopIfGoingOnBatteries = $false
    $exec = $definition.Actions.Create(0)
    $exec.Path = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $exec.Arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $workerPath + '" -Worker -WorkerAction ' + $Action + ' -WorkerAppDirectory "' + $AppDirectory + '" -WorkerOwnerSid ' + $OwnerSid + ' -WorkerPipeName ' + $name + ' -WorkerRequestId ' + $id
    $pipe = [IO.Pipes.NamedPipeServerStream]::new($name, [IO.Pipes.PipeDirection]::In, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    $registered = $false
    try {
        $task = $folder.RegisterTaskDefinition($name, $definition, 2, $OwnerSid, $null, 3, $null)
        $registered = $true
        $connected = $pipe.WaitForConnectionAsync()
        $instance = $task.Run($null)
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        while (-not $connected.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (-not $connected.IsCompleted) { $instance.Stop(); throw 'Desktop camera worker did not connect; registration is unverified.' }
        $connected.GetAwaiter().GetResult()
        $reader = [IO.StreamReader]::new($pipe)
        $reply = $reader.ReadLineAsync()
        while (-not $reply.IsCompleted -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (-not $reply.IsCompleted) { $instance.Stop(); throw 'Desktop camera worker evidence timed out.' }
        $json = $reply.GetAwaiter().GetResult()
        if (-not $json -or $json.Length -gt 1048576) { throw 'Desktop camera worker evidence is empty or oversized.' }
        while ($instance.State -in @(2,4) -and [DateTime]::UtcNow -lt $deadline) {
            $instance.Refresh()
            Start-Sleep -Milliseconds 100
        }
        if ($instance.State -in @(2,4)) { $instance.Stop(); throw 'Desktop camera worker timed out; registration is unverified.' }
        $result = $json | ConvertFrom-Json
        if ($result.requestId -ne $id -or $result.ownerSid -ne $OwnerSid -or $result.appDirectory -ne $AppDirectory -or $result.action -ne $Action -or $result.boundary -ne 'independent-interactive-user' -or $result.workerPid -le 0 -or $result.workerPid -eq $PID) { throw 'Desktop camera worker evidence identity differs.' }
        if (-not $result.ok -or $task.LastTaskResult -ne 0) { throw "Desktop camera worker failed: $($result.error)" }
        return $result
    } finally {
        $pipe.Dispose()
        if ($registered) { $folder.DeleteTask($name, 0) }
    }
}

if ($Worker) {
    $ErrorActionPreference = 'Stop'
    if ($WorkerRequestId -notmatch '^[a-f0-9]{32}$' -or $WorkerOwnerSid -ne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -or
        $WorkerAppDirectory -ne [IO.Path]::GetFullPath($WorkerAppDirectory).TrimEnd('\') -or
        $WorkerPipeName -ne ('CoreVideo-CameraUser-' + $WorkerRequestId)) { throw 'Invalid desktop worker request identity/path.' }
    $result = [ordered]@{requestId=$WorkerRequestId;ownerSid=$WorkerOwnerSid;appDirectory=$WorkerAppDirectory;action=$WorkerAction;
        boundary='independent-interactive-user';workerPid=$PID;workerSessionId=[Diagnostics.Process]::GetCurrentProcess().SessionId;
        observedAtUtc=[DateTime]::UtcNow.ToString('o');ok=$false;error='';userSession=$null}
    try {
        Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
        $result.userSession = Invoke-CameraUserRegistration -Action $WorkerAction -AppDirectory $WorkerAppDirectory
        $result.ok = $true
    } catch { $result.error = $_.Exception.Message }
    $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $WorkerPipeName, [IO.Pipes.PipeDirection]::Out)
    try {
        $pipe.Connect(5000)
        $writer = [IO.StreamWriter]::new($pipe)
        $writer.WriteLine(($result | ConvertTo-Json -Depth 7 -Compress))
        $writer.Flush()
    } finally { $pipe.Dispose() }
    if (-not $result.ok) { exit 1 }
    exit 0
}

