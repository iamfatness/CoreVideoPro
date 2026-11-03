# #781. After a camera DLL upgrade the Windows Frame Server can keep serving the
# OLD CoreVideo camera DLL: an in-proc COM image stays mapped until its service
# host exits, whatever the registry now says (measured: FrameServer PID 29648
# served 8670d079 while registration and core used cd63ab88). This script makes
# such an instance go away so the next camera activation loads the registered
# DLL, and reports serving evidence for Inspect.
#
#   -Action Inspect  read-only; never stops anything.
#   -Action Refresh  waits (bounded) for a Start/Stop Pending service to settle, then
#                    stops FrameServer only when the decision is Stale or Unknown
#                    (Get-FrameServerRefreshDecision), waits for the old instance
#                    to be gone, then restarts it only if it was running (a service
#                    that was still pending counts as running).
#
# Exit codes: 0 = refreshed or not needed (Inspect: always 0 on success);
#             3 = a stale/unknown instance could not be removed (would not stop
#                 in time, or access denied). The caller keeps a correct
#                 registration and warns loudly; 3 is never a rollback trigger;
#             1 = any other error.
# Never kills a process. Never restarts a service that was not running.
#
# 64-BIT REQUIRED. The installer is 32-bit NSIS, and a 32-bit PowerShell cannot
# enumerate the 64-bit service host's modules, so a 32-bit run on 64-bit Windows
# re-invokes itself through Sysnative and relays JSON and exit code.
#
# A refreshed instance loads the DLL lazily on the next activation, so Refresh
# can only prove the stale image is GONE; servingIdentityVerified stays false.
# The proof of the new image is a fresh "Serving module path=" line from the new
# PID in vcam-serve.log (scripts/qa/camera_serving_evidence.py).
[CmdletBinding()]
param([ValidateSet('Inspect','Refresh')][string]$Action = 'Inspect',
    [Parameter(Mandatory=$true)][AllowEmptyString()][string]$ExpectedDll,
    [string]$ServeLog = (Join-Path $env:ProgramData 'CoreVideoPro\vcam-serve.log'),
    # Test seam: scriptblocks replacing the real service calls (see Test-FrameServerRefresh.ps1).
    [hashtable]$Provider = $null,
    # Fault injection for installed tests only: a built-in fake instance, never the real service.
    [ValidateSet('','unstoppable')][string]$Fault = '',
    [int]$StopTimeoutSeconds = 20,
    [int]$StartTimeoutSeconds = 20)
$ErrorActionPreference = 'Stop'
$serviceName = 'FrameServer'

if (-not $Provider -and -not $Fault -and [Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force
    $native = Get-NativePowerShellPath
    $relay = @('-NoProfile','-ExecutionPolicy','Bypass','-File', $PSCommandPath, '-Action', $Action, '-ExpectedDll', $ExpectedDll,
        '-ServeLog', $ServeLog, '-StopTimeoutSeconds', $StopTimeoutSeconds, '-StartTimeoutSeconds', $StartTimeoutSeconds)
    # An empty -ExpectedDll must survive the native argument join as an empty argument.
    $relay = $relay | ForEach-Object { if ($_ -eq '') { '""' } else { $_ } }
    & $native @relay
    exit $LASTEXITCODE
}
Import-Module (Join-Path $PSScriptRoot 'VirtualCameraRegistration.psm1') -Force

function New-DefaultProvider {
    @{
        GetService = {
            $s = Get-CimInstance Win32_Service -Filter "Name='FrameServer'"
            if (-not $s) { return $null }
            $created = ''
            if ($s.ProcessId) {
                $p = Get-CimInstance Win32_Process -Filter ('ProcessId=' + [int]$s.ProcessId)
                if ($p -and $p.CreationDate) { $created = $p.CreationDate.ToUniversalTime().ToString('o') }
            }
            @{ state=[string]$s.State; pid=[int]$s.ProcessId; created=$created }
        }
        GetModules = { param([int]$ProcessId) @(Get-Process -Id $ProcessId -Module -ErrorAction Stop | ForEach-Object FileName) }
        ReadServeLog = {
            param([string]$Path)
            if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
            $item = Get-Item -LiteralPath $Path
            # Bounded: the last 1 MB only; the DLL appends while we read.
            $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite,Delete')
            try {
                $take = [int][Math]::Min($stream.Length, 1MB)
                $null = $stream.Seek(-$take, [IO.SeekOrigin]::End)
                $buffer = New-Object byte[] $take
                $read = 0
                while ($read -lt $take) { $n = $stream.Read($buffer, $read, $take - $read); if ($n -le 0) { break }; $read += $n }
            } finally { $stream.Dispose() }
            @{ lines=@([Text.Encoding]::UTF8.GetString($buffer, 0, $read) -split "`r?`n"); lastWriteUtc=$item.LastWriteTimeUtc.ToString('o') }
        }
        Stop = { Stop-Service -Name 'FrameServer' -Force -ErrorAction Stop }
        Start = { Start-Service -Name 'FrameServer' -ErrorAction Stop }
        Sleep = { param([int]$Milliseconds) Start-Sleep -Milliseconds $Milliseconds }
    }
}

function New-FaultProvider([string]$kind) {
    # 'unstoppable': a running instance holding an old CoreVideo DLL whose Stop
    # is refused. Exercises exit 3 end to end without touching the real service.
    @{
        GetService = { @{ state='Running'; pid=4; created='2000-01-01T00:00:00.0000000Z' } }
        GetModules = { param([int]$ProcessId) @('C:\FaultInjection\' + ('0' * 64) + '\corevideo-virtualcam.dll') }
        ReadServeLog = { param([string]$Path) $null }
        Stop = { throw 'Fault injection: FrameServer refused to stop.' }
        Start = { throw 'Fault injection: Start must not be reached.' }
        Sleep = { param([int]$Milliseconds) }
    }
}

$p = if ($Provider) { $Provider } elseif ($Fault) { New-FaultProvider $Fault } else { New-DefaultProvider }
$watch = [Diagnostics.Stopwatch]::StartNew()

function Get-Instance {
    $s = & $p.GetService
    if (-not $s) { return @{ state='Absent'; pid=0; created='' } }
    return @{ state=[string]$s.state; pid=[int]$s.pid; created=[string]$s.created }
}

function Test-SameInstance($a, $b) { return $a.pid -gt 0 -and $a.pid -eq $b.pid -and $a.created -eq $b.created }

$before = Get-Instance
# A service caught mid-transition (Start Pending / Stop Pending) WAS running for
# restart purposes. Refresh waits, bounded by -StopTimeoutSeconds, for it to settle
# into Running or Stopped and decides from that; if it never settles the decision
# is Unknown and the service is stopped AND restarted (never left stopped under a
# "refreshed" exit 0). Inspect is read-only and never waits.
$pendingStates = @('Start Pending','Stop Pending','Continue Pending','Pause Pending','StartPending','StopPending','ContinuePending','PausePending')
$settledFrom = $null
if ($Action -eq 'Refresh' -and $before.state -in $pendingStates) {
    $settledFrom = $before.state
    $settleWaited = 0
    while ($before.state -in $pendingStates -and $settleWaited -lt $StopTimeoutSeconds * 1000) {
        & $p.Sleep 200; $settleWaited += 200
        $before = Get-Instance
    }
}
$moduleInspection = 'unavailable'; $moduleError = $null; $loaded = @(); $served = @(); $logNote = $null
if ($before.state -eq 'Running' -and $before.pid -gt 0) {
    try { $loaded = @(& $p.GetModules $before.pid); $moduleInspection = 'observed' }
    catch { $moduleError = $_.Exception.Message }
    try {
        $log = & $p.ReadServeLog $ServeLog
        if ($log) {
            # PID reuse guard: a log last written before this instance started
            # cannot contain a line from it, whatever PID the old lines carry.
            $createdAt = [DateTime]::MinValue; $writtenAt = [DateTime]::MinValue
            $haveTimes = [DateTime]::TryParse($before.created, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind, [ref]$createdAt) -and
                [DateTime]::TryParse([string]$log.lastWriteUtc, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind, [ref]$writtenAt)
            if ($haveTimes -and $writtenAt.ToUniversalTime() -lt $createdAt.ToUniversalTime()) { $logNote = 'serve log not written since this FrameServer instance started' }
            else { $served = @(Read-ServeLogModulePaths -Lines @($log.lines) -ProcessId $before.pid) }
        } else { $logNote = 'serve log absent' }
    } catch { $logNote = 'serve log unreadable: ' + $_.Exception.Message }
}
$decision = Get-FrameServerRefreshDecision -ServiceState $before.state -ExpectedDll $ExpectedDll -ModuleInspection $moduleInspection -LoadedPaths $loaded -ServeLogPaths $served
$expectedKey = ConvertTo-CameraDllPathKey $ExpectedDll
$cameraLoaded = @($loaded | Where-Object { Test-CoreVideoCameraDllPath $_ })
$result = [ordered]@{ schema='frameserver-refresh-v1'; action=$Action; expectedDll=$ExpectedDll; decision=$decision; result='not_needed'
    before=[ordered]@{ state=$before.state; pid=$before.pid; created=$before.created }; after=$null
    moduleInspection=$moduleInspection; moduleError=$moduleError; loadedCameraModules=$cameraLoaded
    servedPaths=$served; serveLogNote=$logNote; settledFrom=$settledFrom; restart=$null; error=$null; durationMs=0
    process64=[Environment]::Is64BitProcess; servingIdentityVerified=$false
    limitation='Install time cannot prove the new DLL is serving: Frame Server loads it lazily on the next camera activation.' }

function Write-Result([int]$code) {
    $result.durationMs = [int]$watch.Elapsed.TotalMilliseconds
    $result | ConvertTo-Json -Depth 5 -Compress
    exit $code
}

if ($Action -eq 'Inspect') {
    # Same rule as judge_serving_identity: the current instance's own log line
    # names the expected DLL, and no observed module contradicts it.
    $result.result = 'inspected'
    $result.servingIdentityVerified = [bool]($expectedKey -and $decision -eq 'Current' -and $served.Count -gt 0 -and
        (ConvertTo-CameraDllPathKey $served[-1]) -eq $expectedKey)
    Write-Result 0
}
if ($decision -in @('NotRunning','Current','NotLoaded')) { Write-Result 0 }   # zero service calls

# Anything that reached this point and is not Stopped/Absent (Running, or still
# pending after the bounded settle wait) was running and must come back.
$wasRunning = $before.state -notin @('Stopped','Absent')
try { & $p.Stop }
catch {
    $result.result = 'not_removed'; $result.error = 'Stop failed: ' + $_.Exception.Message
    $result.after = Get-Instance
    Write-Result 3
}
# Gone means: stopped, or Windows already replaced it with a new instance (a
# consumer demand-started FrameServer between our Stop and our poll).
$deadline = $StopTimeoutSeconds * 1000; $waited = 0; $now = Get-Instance
while (-not ($now.state -eq 'Stopped' -or ($now.state -eq 'Running' -and -not (Test-SameInstance $now $before) -and $now.pid -gt 0))) {
    if ($waited -ge $deadline) {
        $result.result = 'not_removed'; $result.error = "FrameServer did not stop within $StopTimeoutSeconds s (state $($now.state))."
        $result.after = $now
        Write-Result 3
    }
    & $p.Sleep 200; $waited += 200
    $now = Get-Instance
}
if ($now.state -eq 'Running') { $result.restart = 'restarted_by_windows' }
elseif ($wasRunning) {
    try {
        & $p.Start
        $waited = 0; $now = Get-Instance
        while (-not ($now.state -eq 'Running' -and -not (Test-SameInstance $now $before))) {
            if ($waited -ge $StartTimeoutSeconds * 1000) { throw "FrameServer did not return to Running within $StartTimeoutSeconds s." }
            & $p.Sleep 200; $waited += 200
            $now = Get-Instance
        }
        $result.restart = 'restarted'
    } catch {
        # The stale image is gone either way; Windows demand-starts FrameServer
        # on the next activation. Loud, but not a failure of the refresh.
        $result.restart = 'left_stopped: ' + $_.Exception.Message
        $now = Get-Instance
    }
} else { $result.restart = 'left_stopped_was_not_running' }
$result.after = [ordered]@{ state=$now.state; pid=$now.pid; created=$now.created }
if (Test-SameInstance $now $before) {
    $result.result = 'not_removed'; $result.error = 'The same FrameServer instance is still running.'
    Write-Result 3
}
$result.result = 'refreshed'
Write-Result 0
