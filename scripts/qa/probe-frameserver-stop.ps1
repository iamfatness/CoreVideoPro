# #781 Task 1: measure what Stop-Service FrameServer actually does, before the
# installer relies on it. Evidence only; no product code depends on this file.
#
#   -Mode Observe   read-only, unelevated is fine: service state/start type/
#                   dependents, PID, creation time, services sharing the host,
#                   whether module enumeration works from THIS host's bitness,
#                   and the serve-log lines for the current PID.
#   -Mode Stop      ELEVATED, owner present: times Stop-Service FrameServer -Force
#                   to Stopped, records which dependents stopped, polls for a
#                   Windows restart, and (with -RestartAfter) restarts what was
#                   running and records the new PID/creation time. Refuses while
#                   CoreVideo is running unless -AllowConsumers is passed (the
#                   probe's point is to measure the camera-open case, so the owner
#                   passes it deliberately with the camera open in a test app).
#
# Run it from BOTH hosts for Step 2:
#   %windir%\SysWOW64\WindowsPowerShell\v1.0\powershell.exe -File probe-frameserver-stop.ps1
#   %windir%\System32\WindowsPowerShell\v1.0\powershell.exe -File probe-frameserver-stop.ps1
# Never kills a process. Writes JSON to stdout and, with -OutDir, a copy there
# (never on C: by repo practice; pass a D:\ artifacts path).
[CmdletBinding()]
param([ValidateSet('Observe','Stop')][string]$Mode = 'Observe',
    [switch]$RestartAfter, [switch]$AllowConsumers,
    [int]$StopTimeoutSeconds = 20,
    [string]$ServeLog = (Join-Path $env:ProgramData 'CoreVideoPro\vcam-serve.log'),
    [string]$OutDir = '')
$ErrorActionPreference = 'Stop'

function Get-Identity([string]$name) {
    $s = Get-CimInstance Win32_Service -Filter "Name='$name'"
    if (-not $s) { return [ordered]@{ name=$name; present=$false } }
    $created = $null
    if ($s.ProcessId) {
        $p = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $s.ProcessId)
        if ($p -and $p.CreationDate) { $created = $p.CreationDate.ToUniversalTime().ToString('o') }
    }
    $shared = @()
    if ($s.ProcessId) { $shared = @(Get-CimInstance Win32_Service -Filter ("ProcessId=" + $s.ProcessId) | ForEach-Object Name) }
    $svc = Get-Service -Name $name
    [ordered]@{ name=$name; present=$true; state=[string]$s.State; startMode=[string]$s.StartMode; pid=[int]$s.ProcessId
        created=$created; sharedHostServices=$shared
        dependents=@($svc.DependentServices | ForEach-Object { [ordered]@{ name=$_.Name; status=[string]$_.Status } })
        dependsOn=@($svc.ServicesDependedOn | ForEach-Object Name) }
}

function Get-ModuleProbe([int]$processId) {
    if ($processId -le 0) { return [ordered]@{ result='no-process' } }
    try {
        $mods = @(Get-Process -Id $processId -Module -ErrorAction Stop)
        [ordered]@{ result='observed'; moduleCount=$mods.Count
            corevideo=@($mods | Where-Object ModuleName -eq 'corevideo-virtualcam.dll' | ForEach-Object FileName) }
    } catch { [ordered]@{ result='unavailable'; error=$_.Exception.Message } }
}

function Get-ServeLines([int]$processId) {
    if ($processId -le 0 -or -not (Test-Path -LiteralPath $ServeLog -PathType Leaf)) { return @() }
    $item = Get-Item -LiteralPath $ServeLog
    $stream = [IO.File]::Open($ServeLog, 'Open', 'Read', 'ReadWrite,Delete')
    try {
        $take = [Math]::Min($stream.Length, 1MB)
        $null = $stream.Seek(-$take, 'End')
        $buffer = New-Object byte[] $take
        $null = $stream.Read($buffer, 0, $take)
    } finally { $stream.Dispose() }
    $pattern = '^\[\d{2}:\d{2}:\d{2}\.\d{3} pid=' + $processId + '\] Serving module path=(.+)$'
    @([Text.Encoding]::UTF8.GetString($buffer) -split "`r?`n" | Where-Object { $_ -match $pattern } | Select-Object -Last 5) +
        @("logLastWriteUtc=" + $item.LastWriteTimeUtc.ToString('o'))
}

$isAdmin = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$report = [ordered]@{ schema='frameserver-stop-probe-v1'; mode=$Mode; timeUtc=[DateTime]::UtcNow.ToString('o')
    host=[ordered]@{ is64BitOS=[Environment]::Is64BitOperatingSystem; is64BitProcess=[Environment]::Is64BitProcess; pshome=$PSHOME; admin=$isAdmin }
    consumers=@(Get-Process -Name 'CoreVideoPro.WinUI','corevideo-native','Zoom' -ErrorAction SilentlyContinue | ForEach-Object { [ordered]@{ name=$_.Name; pid=$_.Id } }) }
$before = Get-Identity 'FrameServer'
$report.before = $before
$report.monitorBefore = Get-Identity 'FrameServerMonitor'
$report.moduleProbe = Get-ModuleProbe $before.pid
$report.serveLogForPid = Get-ServeLines $before.pid

if ($Mode -eq 'Stop') {
    if (-not $isAdmin) { throw 'Stop mode requires an elevated 64-bit PowerShell.' }
    if (-not [Environment]::Is64BitProcess) { throw 'Stop mode must run in 64-bit PowerShell (System32), not SysWOW64.' }
    if ($report.consumers.Count -and -not $AllowConsumers) { throw 'Camera consumers are running; pass -AllowConsumers only with the owner present.' }
    $wasRunning = @(@('FrameServer','FrameServerMonitor') | Where-Object { (Get-Service -Name $_ -ErrorAction SilentlyContinue).Status -eq 'Running' })
    $report.wasRunning = $wasRunning
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try { Stop-Service -Name FrameServer -Force -ErrorAction Stop; $report.stopError = $null }
    catch { $report.stopError = $_.Exception.Message }
    $stopped = $false
    while ($watch.Elapsed.TotalSeconds -lt $StopTimeoutSeconds) {
        if ((Get-Service FrameServer).Status -eq 'Stopped') { $stopped = $true; break }
        Start-Sleep -Milliseconds 100
    }
    $report.stopMs = [int]$watch.Elapsed.TotalMilliseconds
    $report.stopped = $stopped
    $report.oldProcessGone = -not (Get-Process -Id $before.pid -ErrorAction SilentlyContinue) -or $before.pid -le 0
    $report.afterStop = Get-Identity 'FrameServer'
    $report.monitorAfterStop = Get-Identity 'FrameServerMonitor'
    # Does Windows (or the open consumer) demand-start it again on its own?
    $restartWatch = [Diagnostics.Stopwatch]::StartNew(); $selfRestart = $null
    while ($restartWatch.Elapsed.TotalSeconds -lt 10) {
        $now = Get-Identity 'FrameServer'
        if ($now.state -eq 'Running') { $selfRestart = $now; break }
        Start-Sleep -Milliseconds 250
    }
    $report.windowsRestartedWithin10s = $selfRestart
    if ($RestartAfter -and -not $selfRestart) {
        foreach ($name in $wasRunning) { Start-Service -Name $name }
    }
    $report.after = Get-Identity 'FrameServer'
    $report.creationChanged = ($report.after.created -ne $before.created)
    $report.next = 'Activate the camera, then rerun -Mode Observe: a fresh "Serving module path=" line from after.pid proves the new load.'
}
$json = $report | ConvertTo-Json -Depth 6
$json
if ($OutDir) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $OutDir ("frameserver-probe-{0}-{1}.json" -f $Mode, [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss'))), $json, (New-Object Text.UTF8Encoding $false))
}
