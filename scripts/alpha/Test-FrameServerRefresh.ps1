# #781: Refresh-FrameServer.ps1 against a fake service provider. No real service
# is stopped and the production vcam-serve.log is never read or written: every
# case passes -Provider, and the one real-provider case is a read-only Inspect.
# Run from both hosts (CI runs both):
#   %windir%\System32\WindowsPowerShell\v1.0\powershell.exe -File Test-FrameServerRefresh.ps1
#   %windir%\SysWOW64\WindowsPowerShell\v1.0\powershell.exe -File Test-FrameServerRefresh.ps1
param([switch]$IncludeInstalledInspect)
$ErrorActionPreference = 'Stop'
$script =Join-Path $PSScriptRoot 'Refresh-FrameServer.ps1'
$checks = 0
function Assert([bool]$c, [string]$m) { if (!$c) { throw $m }; $script:checks++ }
$expected = 'C:\Program Files\Common Files\CoreVideoProCamera\' + ('b' * 64) + '\corevideo-virtualcam.dll'
$old = 'C:\Program Files\Common Files\CoreVideoProCamera\' + ('a' * 64) + '\corevideo-virtualcam.dll'
$t0 = '2026-10-23T10:00:00.0000000Z'; $t1 = '2026-10-23T10:05:00.0000000Z'

# A scripted fake: $state.instances is the sequence GetService walks through
# after each Stop/Start/Sleep; calls are counted so "no service call" is provable.
function New-Fake([hashtable]$opts) {
    $state = @{ calls=[Collections.Generic.List[string]]::new(); current=$opts.initial; afterStop=$opts.afterStop
        afterStart=$opts.afterStart; stopThrows=$opts.stopThrows; startThrows=$opts.startThrows; modules=$opts.modules
        modulesThrow=$opts.modulesThrow; log=$opts.log; stopsAfterSleeps=$opts.stopsAfterSleeps; sleeps=0
        # A pending service settles to settleTo after settlesAfterSleeps sleeps (once).
        settlesAfterSleeps=$opts.settlesAfterSleeps; settleTo=$opts.settleTo }
    $provider = @{
        GetService = { $state.calls.Add('GetService'); if ($null -eq $state.current) { return $null }; $state.current }.GetNewClosure()
        GetModules = { param([int]$ProcessId) $state.calls.Add('GetModules'); if ($state.modulesThrow) { throw 'Access is denied' }; @($state.modules) }.GetNewClosure()
        ReadServeLog = { param([string]$Path) $state.calls.Add('ReadServeLog'); $state.log }.GetNewClosure()
        Stop = { $state.calls.Add('Stop'); if ($state.stopThrows) { throw $state.stopThrows }
                 if ($null -eq $state.stopsAfterSleeps) { $state.current = $state.afterStop } }.GetNewClosure()
        Start = { $state.calls.Add('Start'); if ($state.startThrows) { throw $state.startThrows }; $state.current = $state.afterStart }.GetNewClosure()
        Sleep = { param([int]$Milliseconds) $state.calls.Add('Sleep'); $state.sleeps++
                  if ($null -ne $state.settlesAfterSleeps -and $null -ne $state.settleTo -and $state.sleeps -ge $state.settlesAfterSleeps) { $state.current = $state.settleTo; $state.settleTo = $null }
                  if ($null -ne $state.stopsAfterSleeps -and $state.sleeps -ge $state.stopsAfterSleeps) { $state.current = $state.afterStop } }.GetNewClosure()
        Kill = { $state.calls.Add('Kill') }.GetNewClosure()
    }
    return @{ provider=$provider; state=$state }
}
function Invoke-Refresh([hashtable]$fake, [string]$action = 'Refresh', [string]$dll = $expected, [int]$stopTimeout = 2) {
    $out = & $script -Action $action -ExpectedDll $dll -ServeLog 'X:\not-used\vcam-serve.log' -Provider $fake.provider -StopTimeoutSeconds $stopTimeout -StartTimeoutSeconds 2
    return @{ code=$LASTEXITCODE; json=(($out -join "`n") | ConvertFrom-Json); calls=@($fake.state.calls) }
}
function Count([string[]]$calls, [string]$name) { @($calls | Where-Object { $_ -eq $name }).Count }
$running = @{ state='Running'; pid=4242; created=$t0 }
$replacement = @{ state='Running'; pid=5151; created=$t1 }
$stopped = @{ state='Stopped'; pid=0; created='' }
$logFor = { param($lines, $written = $t1) @{ lines=$lines; lastWriteUtc=$written } }

# 1. Stale (old module loaded) -> stop, wait, start, new instance -> refreshed, exit 0.
$f = New-Fake @{ initial=$running; modules=@('C:\Windows\System32\x.dll',$old); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.result -eq 'refreshed' -and $r.json.decision -eq 'Stale') "stale: $($r.code) $($r.json.result)"
Assert ((Count $r.calls 'Stop') -eq 1 -and (Count $r.calls 'Start') -eq 1) 'stale: expected one Stop and one Start'
Assert ($r.json.before.created -ne $r.json.after.created -and $r.json.after.pid -eq 5151 -and $r.json.restart -eq 'restarted') 'stale: creation time did not change'
Assert (-not $r.json.servingIdentityVerified) 'Refresh claimed the new DLL is serving (it loads lazily).'
Assert ($r.json.schema -eq 'frameserver-refresh-v1') 'schema'

# 2. Will not stop within the bounded wait -> exit 3, never a kill.
$f = New-Fake @{ initial=$running; modules=@($old); afterStop=$running; stopsAfterSleeps=100000 }
$r = Invoke-Refresh $f 'Refresh' $expected 1
Assert ($r.code -eq 3 -and $r.json.result -eq 'not_removed' -and $r.json.error -match 'did not stop') "unstoppable: $($r.code) $($r.json.error)"
Assert ((Count $r.calls 'Kill') -eq 0 -and (Count $r.calls 'Start') -eq 0) 'unstoppable: killed or restarted'
Assert ((Count $r.calls 'Sleep') -le 6) 'unstoppable: wait was not bounded by the timeout'
Assert (-not ((Get-Content -LiteralPath $script -Raw) -match '(?i)Stop-Process|taskkill|\.Kill\(')) 'Refresh-FrameServer.ps1 must never kill a process.'

# 3. Stop throws access denied -> exit 3 with the message.
$f = New-Fake @{ initial=$running; modules=@($old); stopThrows='Access is denied.' }
$r = Invoke-Refresh $f
Assert ($r.code -eq 3 -and $r.json.error -match 'Access is denied' -and (Count $r.calls 'Start') -eq 0) "access denied: $($r.code) $($r.json.error)"

# 4. Windows (a consumer) restarts the service between our Stop and our poll -> refreshed, no Start.
$f = New-Fake @{ initial=$running; modules=@($old); afterStop=$replacement }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.result -eq 'refreshed' -and $r.json.restart -eq 'restarted_by_windows' -and (Count $r.calls 'Start') -eq 0) "replaced: $($r.code) $($r.json.restart)"

# 5. Slow stop inside the bound -> polls, then refreshed.
$f = New-Fake @{ initial=$running; modules=@($old); afterStop=$stopped; afterStart=$replacement; stopsAfterSleeps=3 }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.result -eq 'refreshed' -and (Count $r.calls 'Sleep') -ge 3) 'slow stop'

# 6. Restart fails after the stale instance is gone -> still refreshed (Windows demand-starts), reported loudly.
$f = New-Fake @{ initial=$running; modules=@($old); afterStop=$stopped; startThrows='Start refused' }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.result -eq 'refreshed' -and $r.json.restart -match '^left_stopped: .*Start refused') "restart failure: $($r.json.restart)"

# 7. A stopped service stays stopped: zero Stop/Start calls.
$f = New-Fake @{ initial=$stopped }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.decision -eq 'NotRunning' -and $r.json.result -eq 'not_needed') 'stopped'
Assert ((Count $r.calls 'Stop') + (Count $r.calls 'Start') + (Count $r.calls 'GetModules') -eq 0) 'stopped: service was touched'
$f = New-Fake @{ initial=$null }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.decision -eq 'NotRunning' -and (Count $r.calls 'Stop') -eq 0) 'absent service'

# 8. Current and NotLoaded: zero Stop/Start calls (the decision is load-bearing).
foreach ($case in @(@{ modules=@($expected); decision='Current' }, @{ modules=@('C:\Windows\System32\x.dll'); decision='NotLoaded' })) {
    $f = New-Fake @{ initial=$running; modules=$case.modules; afterStop=$stopped; afterStart=$replacement }
    $r = Invoke-Refresh $f
    Assert ($r.code -eq 0 -and $r.json.decision -eq $case.decision -and $r.json.result -eq 'not_needed') "$($case.decision): $($r.json.decision)"
    Assert ((Count $r.calls 'Stop') -eq 0 -and (Count $r.calls 'Start') -eq 0) "$($case.decision): service was restarted"
}

# 9. Unknown (module API denied, no serve-log line for this PID) refreshes.
$f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor @("[10:01:00.000 pid=999] Serving module path=$expected")); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f
Assert ($r.json.decision -eq 'Unknown' -and $r.json.moduleInspection -eq 'unavailable' -and $r.json.result -eq 'refreshed' -and (Count $r.calls 'Stop') -eq 1) "unknown: $($r.json.decision)"
Assert ($r.json.moduleError -match 'Access is denied') 'unknown: module error not reported'

# 10. Module API denied, serve log names the old DLL for this PID -> Stale; names the expected one -> Current.
$f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor @("[10:01:00.000 pid=4242] Serving module path=$old")); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f
Assert ($r.json.decision -eq 'Stale' -and $r.json.result -eq 'refreshed') 'serve-log stale'
$f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor @("[10:01:00.000 pid=4242] Serving module path=$old", "[10:02:00.000 pid=4242] Serving module path=$expected")) }
$r = Invoke-Refresh $f
Assert ($r.json.decision -eq 'Current' -and (Count $r.calls 'Stop') -eq 0) 'serve-log current'

# 11. PID-reuse guard: a log last written before this instance started is no evidence.
$f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor @("[10:01:00.000 pid=4242] Serving module path=$expected") '2026-10-23T09:00:00.0000000Z'); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f
Assert ($r.json.decision -eq 'Unknown' -and $r.json.serveLogNote -match 'not written since') "pid reuse: $($r.json.decision)"

# 12. Uninstall: nothing registered, a CoreVideo camera still loaded -> refresh.
$f = New-Fake @{ initial=$running; modules=@($expected); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f 'Refresh' ''
Assert ($r.json.decision -eq 'Stale' -and $r.json.result -eq 'refreshed') 'uninstall stale'

# 13. Inspect never stops anything, and verifies serving only from this PID's own line.
$f = New-Fake @{ initial=$running; modules=@($old); afterStop=$stopped }
$r = Invoke-Refresh $f 'Inspect'
Assert ($r.code -eq 0 -and $r.json.result -eq 'inspected' -and (Count $r.calls 'Stop') -eq 0 -and -not $r.json.servingIdentityVerified) 'inspect stale'
$f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor @("[10:02:00.000 pid=4242] Serving module path=$expected")) }
$r = Invoke-Refresh $f 'Inspect'
Assert ($r.json.servingIdentityVerified -and $r.json.decision -eq 'Current') 'inspect: own-PID expected line did not verify'
foreach ($lines in @(@("[10:02:00.000 pid=4242] Serving module path=$old"), @("[10:02:00.000 pid=999] Serving module path=$expected"), @())) {
    $f = New-Fake @{ initial=$running; modulesThrow=$true; log=(& $logFor $lines) }
    $r = Invoke-Refresh $f 'Inspect'
    Assert (-not $r.json.servingIdentityVerified) "inspect verified without own-PID evidence: $($lines -join ';')"
}
# Loaded modules alone (no activation line yet) never verify serving.
$f = New-Fake @{ initial=$running; modules=@($expected); log=(& $logFor @()) }
$r = Invoke-Refresh $f 'Inspect'
Assert ($r.json.decision -eq 'Current' -and -not $r.json.servingIdentityVerified) 'inspect: module alone verified serving'

# 14. Built-in fault injection (used by the installed test) exits 3 without touching the real service.
$out = & $script -Action Refresh -ExpectedDll $expected -Fault unstoppable
$j = ($out -join "`n") | ConvertFrom-Json
Assert ($LASTEXITCODE -eq 3 -and $j.result -eq 'not_removed' -and $j.error -match 'Fault injection') "fault: $LASTEXITCODE"

# 15. Real provider, read-only Inspect, from a child of THIS host: from SysWOW64 it must relay through Sysnative.
$hostExe = Join-Path $PSHOME 'powershell.exe'
$out = & $hostExe -NoProfile -ExecutionPolicy Bypass -File $script -Action Inspect -ExpectedDll $expected -ServeLog (Join-Path ([IO.Path]::GetTempPath()) ('cvp-no-log-' + [Guid]::NewGuid().ToString('N') + '.log'))
Assert ($LASTEXITCODE -eq 0) "real inspect exited $LASTEXITCODE"
$j = ($out -join "`n") | ConvertFrom-Json
Assert ($j.process64 -eq [Environment]::Is64BitOperatingSystem) "real inspect ran in a $(if ($j.process64) {'64'} else {'32'})-bit process"
Assert (-not $j.servingIdentityVerified -and $j.result -eq 'inspected') 'real inspect claimed serving without a log'

# 16. (opt-in: needs the independent desktop-user worker, so not on CI) the real,
# read-only Install-VirtualCamera.ps1 -Action Inspect now carries Frame Server
# evidence, and an empty temp serve log never verifies serving identity.
if ($IncludeInstalledInspect) {
    $app = [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $PSScriptRoot))).TrimEnd('\')
    $emptyLog = Join-Path ([IO.Path]::GetTempPath()) ('cvp-empty-serve-' + [Guid]::NewGuid().ToString('N') + '.log')
    [IO.File]::WriteAllText($emptyLog, '')
    try {
        $out = & $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Install-VirtualCamera.ps1') -Action Inspect -AppDirectory $app -ServeLog $emptyLog
        Assert ($LASTEXITCODE -eq 0) "installed inspect exited $LASTEXITCODE"
        $j = ($out -join "`n") | ConvertFrom-Json
        Assert ($j.frameServer.schema -eq 'frameserver-refresh-v1' -and $j.frameServer.process64 -eq [Environment]::Is64BitOperatingSystem) 'installed inspect: no 64-bit Frame Server evidence'
        Assert ($j.moduleInspection -in @('observed','unverified') -and ($j.moduleInspection -eq 'observed' -or $j.moduleInspectionReason)) 'installed inspect: unverified without a reason'
        Assert (-not $j.servingIdentityVerified) 'installed inspect: verified serving from an empty serve log'
    } finally { [IO.File]::Delete($emptyLog) }
}

# 17. Pending states (#781 review). A service caught mid-transition WAS running for
# restart purposes: before, Stop Pending/Start Pending mapped to Unknown, Stop was
# issued, and $wasRunning=false left the service stopped under a "refreshed" exit 0.
$stopPending = @{ state='Stop Pending'; pid=4242; created=$t0 }
$startPending = @{ state='Start Pending'; pid=4242; created=$t0 }
# a. Never settles within the bound -> Unknown, Stop, then Start (never left stopped).
$f = New-Fake @{ initial=$stopPending; modules=@($old); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f 'Refresh' $expected 1
Assert ($r.code -eq 0 -and $r.json.result -eq 'refreshed' -and $r.json.restart -eq 'restarted' -and (Count $r.calls 'Start') -eq 1) "stop pending unsettled: $($r.code) $($r.json.result) $($r.json.restart)"
Assert ((Count $r.calls 'Sleep') -le 6) 'stop pending: the settle wait was not bounded by the timeout'
# b. Start Pending settles to Running with the old DLL -> Stale -> refreshed and restarted.
$f = New-Fake @{ initial=$startPending; settlesAfterSleeps=2; settleTo=$running; modules=@($old); afterStop=$stopped; afterStart=$replacement }
$r = Invoke-Refresh $f
Assert ($r.json.decision -eq 'Stale' -and $r.json.result -eq 'refreshed' -and $r.json.restart -eq 'restarted' -and (Count $r.calls 'Start') -eq 1) "start pending settled: $($r.json.decision) $($r.json.restart)"
Assert ($r.json.settledFrom -eq 'Start Pending') "start pending: settledFrom not reported ($($r.json.settledFrom))"
# c. Stop Pending settles to Stopped -> NotRunning, zero service calls.
$f = New-Fake @{ initial=$stopPending; settlesAfterSleeps=1; settleTo=$stopped }
$r = Invoke-Refresh $f
Assert ($r.code -eq 0 -and $r.json.decision -eq 'NotRunning' -and (Count $r.calls 'Stop') + (Count $r.calls 'Start') -eq 0) "stop pending settled stopped: $($r.json.decision)"
# d. Inspect stays read-only and immediate on a pending service: no wait, no Stop.
$f = New-Fake @{ initial=$stopPending }
$r = Invoke-Refresh $f 'Inspect'
Assert ($r.code -eq 0 -and $r.json.decision -eq 'Unknown' -and (Count $r.calls 'Sleep') -eq 0 -and (Count $r.calls 'Stop') -eq 0) 'inspect on a pending service waited or stopped it'

Write-Output "Test-FrameServerRefresh passed ($checks checks; host 64-bit: $([Environment]::Is64BitProcess)). No real service was stopped."
