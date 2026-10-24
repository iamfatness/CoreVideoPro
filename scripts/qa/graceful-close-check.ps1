<#
.SYNOPSIS
  The standing graceful-close check (#457): close a long-running CoreVideo Pro
  ONCE with WM_CLOSE and judge whether it ended on the clean TerminateProcess
  path with no crash.

.DESCRIPTION
  #457 (dump 17304) was the post-Exit dispatcher drain: a graceful close after a
  long in-meeting session finished its cleanup, handed torn-down XAML back to
  WinUI and fail-fasted (0xc000027b, CoreMessagingXP). #462 fixed it by ending
  the process with TerminateProcess after a clean shutdown
  (ShutdownCompletion.Complete). This check turns "no recurrence" from an
  inference over old logs into a gate. Run it at the end of the RC soak, after
  the app has been in the test meeting for 30 minutes or more, Engine on, with
  no recording and no stream.

  Pass = all of:
    - this session's last launch.log line is
      "shutdown: cleanup complete; terminating the process directly";
    - the process exited within 6 s of WM_CLOSE;
    - no new CoreVideoPro.WinUI.exe.*.dmp in %LOCALAPPDATA%\CrashDumps or
      %LOCALAPPDATA%\CoreVideoPro\crashes;
    - no new Application event 1000 for CoreVideoPro.WinUI.exe within 60 s.

  Safety (owner rules): no SendKeys, no focus stealing, no UI Automation. The
  only interaction is the control API (read-only GET /state and /snapshot) and
  ONE WM_CLOSE (Process.CloseMainWindow). The check refuses while recording or
  streaming is live or finalizing (Test-DrillSafeToClose from
  outputs-drill-lib.ps1), because the shell's close guard would prompt and
  activate its window. It never sends a second close: a second close during
  cleanup makes the shell take the ApplicationLifecycle.ForceExit path, which is
  a different exit than the one being judged. Evidence goes to D: only.

  Exit codes: 0 pass, 1 fail, 2 refused (nothing was closed).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\graceful-close-check.ps1
#>
param(
  [string]$Out = (Join-Path 'D:\CoreVideo-QA\graceful-close' (Get-Date -Format 'yyyyMMdd-HHmmss')),
  [int]$MinSessionMinutes = 30,
  [switch]$AllowOutsideMeeting,
  [string]$Api = 'http://127.0.0.1:8011'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')

$script:TerminateMarker = 'shutdown: cleanup complete; terminating the process directly'
$script:ExitBudgetMs = 6000
$script:EventWindowSeconds = 60

function Test-GracefulCloseOut([string]$Path) {
  $problems = @()
  if ($Path -notmatch '^[Dd]:\\') { $problems += "evidence folder must be an absolute path on D: (got '$Path')" }
  return $problems   # callers wrap in @()
}

function Get-GracefulCloseSession {
  # The shell's session id from the last launch.log line written by this pid.
  # PIDs are reused across days, so everything after this keys on the session.
  param([string[]]$Lines, [int]$ProcessId)
  $session = $null
  foreach ($line in $Lines) {
    if ($line -match "\[pid=$ProcessId session=([0-9a-fA-F]+) ") { $session = $Matches[1] }
  }
  return $session
}

function Get-GracefulCloseLastLine {
  param([string[]]$Lines, [string]$Session)
  if (-not $Session) { return $null }
  $last = $null
  foreach ($line in $Lines) { if ($line -like "*session=$Session *") { $last = $line } }
  return $last
}

function Get-GracefulCloseRefusals {
  param($State, $Snapshot, [datetime]$ProcessStart, [datetime]$Now, [int]$MinSessionMinutes, [switch]$AllowOutsideMeeting)
  # Reuse the outputs drill's close guard. It also lists 'engine on' because the
  # drill turns the engine off before closing; the shell's close guard
  # (CloseGuardPolicy) only prompts for recording and streaming, and this check
  # is meant to close with the engine on, as an operator would.
  $reasons = @(Test-DrillSafeToClose $State $Snapshot | Where-Object { $_ -ne 'engine on' })
  if ($null -eq $State -or $null -eq $Snapshot) { return $reasons }
  $upMinutes = ($Now - $ProcessStart).TotalMinutes
  if ($upMinutes -lt $MinSessionMinutes) { $reasons += ('shell up {0:N1} min, below the {1} min minimum' -f $upMinutes, $MinSessionMinutes) }
  if (-not $AllowOutsideMeeting -and $Snapshot.meetingState -ne 'in_meeting') { $reasons += "not in a meeting (meetingState '$($Snapshot.meetingState)')" }
  return $reasons   # callers wrap in @()
}

function Get-GracefulCloseVerdict {
  param([string]$LastLine, [bool]$Exited, $ExitMs, [string[]]$NewDumps = @(), [object[]]$NewEvents = @())
  $reasons = @()
  $terminate = [bool]($LastLine -and $LastLine.Contains($script:TerminateMarker))
  if (-not $LastLine) { $reasons += 'no launch.log line found for this session' }
  elseif (-not $terminate) { $reasons += "last log line is not '$($script:TerminateMarker)': $LastLine" }
  if (-not $Exited) { $reasons += 'process did not exit' }
  elseif ($null -eq $ExitMs -or [int]$ExitMs -gt $script:ExitBudgetMs) { $reasons += "exit took $ExitMs ms (budget $($script:ExitBudgetMs) ms)" }
  if (@($NewDumps).Count) { $reasons += "new shell dump(s): $(@($NewDumps) -join ', ')" }
  if (@($NewEvents).Count) { $reasons += "new Application event 1000 for CoreVideoPro.WinUI.exe: $(@($NewEvents).Count)" }
  return [ordered]@{
    pass = ($reasons.Count -eq 0)
    reasons = @($reasons)
    terminateLine = $terminate
    lastLine = $LastLine
    exitMs = $ExitMs
    newDumps = @($NewDumps)
    newEvent1000 = @($NewEvents)
  }
}

function Get-GracefulCloseDumpFolders {
  @((Join-Path $env:LOCALAPPDATA 'CrashDumps'), (Join-Path $env:LOCALAPPDATA 'CoreVideoPro\crashes'))
}

function Get-GracefulCloseDumps {
  @(foreach ($folder in Get-GracefulCloseDumpFolders) {
      Get-ChildItem -LiteralPath $folder -Filter 'CoreVideoPro.WinUI.exe.*.dmp' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    })
}

function Get-GracefulCloseEvents([datetime]$Since) {
  @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $Since } -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -like '*CoreVideoPro.WinUI.exe*' } |
      ForEach-Object {
        $m = $_.Message
        [pscustomobject]@{
          time = $_.TimeCreated.ToString('o')
          module = if ($m -match 'Faulting module name: ([^,\r\n]+)') { $Matches[1] } else { $null }
          code = if ($m -match 'Exception code: (0x[0-9a-fA-F]+)') { $Matches[1] } else { $null }
          offset = if ($m -match 'Fault offset: (0x[0-9a-fA-F]+)') { $Matches[1] } else { $null }
          processId = if ($m -match 'Faulting process id: (0x[0-9a-fA-F]+)') { $Matches[1] } else { $null }
        }
      })
}

function Read-GracefulCloseLog([string]$Path) {
  # The shell may still hold the file; open it shared rather than locking it.
  if (-not (Test-Path -LiteralPath $Path)) { return @() }
  $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
  try {
    $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
    return @($reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ })
  } finally { $stream.Dispose() }
}

# Dot-sourced by the test: define the functions above and stop here.
if ($MyInvocation.InvocationName -eq '.') { return }

# ---------------------------------------------------------------- preflight
$problems = @(Test-GracefulCloseOut $Out)
if ($problems.Count) { Write-Host ('REFUSED: ' + ($problems -join '; ')) -ForegroundColor Yellow; exit 2 }
$null = New-Item -ItemType Directory -Force -Path $Out
$logPath = Join-Path $env:LOCALAPPDATA 'CoreVideoPro\launch.log'

function Write-CheckLog([string]$Message) {
  $line = '[' + [DateTime]::UtcNow.ToString('o') + '] ' + $Message
  Write-Host $line
  Add-Content -LiteralPath (Join-Path $Out 'check.log') -Value $line -Encoding UTF8
}
function Write-CheckJson($Value, [string]$Name) {
  $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Out $Name) -Encoding UTF8
}
function Refuse([string[]]$Reasons) {
  Write-CheckLog ('REFUSED, nothing was closed: ' + ($Reasons -join '; '))
  Write-CheckJson ([ordered]@{ refused = $true; reasons = @($Reasons); utc = [DateTime]::UtcNow.ToString('o') }) 'verdict.json'
  exit 2
}

$shells = @(Get-Process CoreVideoPro.WinUI -ErrorAction SilentlyContinue)
if ($shells.Count -ne 1) { Refuse @("expected exactly one CoreVideoPro.WinUI process, found $($shells.Count)") }
$app = $shells[0]
if ($app.MainWindowHandle -eq [IntPtr]::Zero) { Refuse @("pid $($app.Id) has no main window to send WM_CLOSE to") }

try {
  $state = Invoke-RestMethod "$Api/state" -TimeoutSec 5
  $snapshot = (Invoke-RestMethod "$Api/snapshot" -TimeoutSec 5).snapshot
} catch { $state = $null; $snapshot = $null }
$refusals = @(Get-GracefulCloseRefusals -State $state -Snapshot $snapshot -ProcessStart $app.StartTime -Now (Get-Date) -MinSessionMinutes $MinSessionMinutes -AllowOutsideMeeting:$AllowOutsideMeeting)
if ($refusals.Count) { Refuse $refusals }

$session = Get-GracefulCloseSession -Lines (Read-GracefulCloseLog $logPath) -ProcessId $app.Id
if (-not $session) { Refuse @("no launch.log line for pid $($app.Id) in $logPath") }
$dumpsBefore = Get-GracefulCloseDumps
$sessionMinutes = [math]::Round(((Get-Date) - $app.StartTime).TotalMinutes, 1)
$build = $null
foreach ($line in (Read-GracefulCloseLog $logPath)) { if ($line -like "*session=$session *" -and $line -match 'build=([^ ]+)') { $build = $Matches[1]; break } }
Write-CheckJson ([ordered]@{ pid = $app.Id; session = $session; build = $build; started = $app.StartTime.ToString('o'); sessionMinutes = $sessionMinutes
    meetingState = $snapshot.meetingState; engineOn = $state.engineOn; dumpsBefore = @($dumpsBefore) }) 'before.json'
Write-CheckLog "Closing pid $($app.Id) session $session build $build after $sessionMinutes min (one WM_CLOSE)"

# ---------------------------------------------------------------- close once
$closeAt = Get-Date
$eventsSince = $closeAt.AddSeconds(-1)
$watch = [Diagnostics.Stopwatch]::StartNew()
if (-not $app.CloseMainWindow()) { Refuse @('CloseMainWindow returned false; WM_CLOSE was not posted') }
$exited = $app.WaitForExit($script:ExitBudgetMs)
$exitMs = if ($exited) { [int]$watch.ElapsedMilliseconds } else { $null }
if (-not $exited) {
  # Never send a second close (that takes the ForceExit path). Wait to learn how long it really took.
  Write-CheckLog "Not exited after $($script:ExitBudgetMs) ms; waiting up to 60 s more without sending anything"
  if ($app.WaitForExit(60000)) { $exited = $true; $exitMs = [int]$watch.ElapsedMilliseconds }
}
Write-CheckLog $(if ($exited) { "Exited after $exitMs ms" } else { 'Still running after 66 s' })

# ---------------------------------------------------------------- crash window
$remaining = $script:EventWindowSeconds - ((Get-Date) - $closeAt).TotalSeconds
if ($remaining -gt 0) { Write-CheckLog ('Watching {0:N0} s more for a dump or event 1000' -f $remaining); Start-Sleep -Seconds ([math]::Ceiling($remaining)) }
$newDumps = @(Get-GracefulCloseDumps | Where-Object { $dumpsBefore -notcontains $_ })
$newEvents = @(Get-GracefulCloseEvents $eventsSince)
$lastLine = Get-GracefulCloseLastLine -Lines (Read-GracefulCloseLog $logPath) -Session $session

$verdict = Get-GracefulCloseVerdict -LastLine $lastLine -Exited $exited -ExitMs $exitMs -NewDumps $newDumps -NewEvents $newEvents
$verdict['pid'] = $app.Id; $verdict['session'] = $session; $verdict['build'] = $build; $verdict['sessionMinutes'] = $sessionMinutes
$verdict['closedUtc'] = $closeAt.ToUniversalTime().ToString('o')
Write-CheckJson $verdict 'verdict.json'
if ($newDumps.Count) {
  Write-Host "!!! New shell dump(s). Copy them, the exact publish folder (exe, PDBs, DLLs) and launch/media-core/perf logs to D:\CoreVideo-QA\dumps\<issue>\ BEFORE any rebuild." -ForegroundColor Red
}
Write-CheckLog ("Verdict: " + $(if ($verdict.pass) { 'PASS' } else { 'FAIL: ' + ($verdict.reasons -join '; ') }) + " (see $(Join-Path $Out 'verdict.json'))")
if ($verdict.pass) { exit 0 } else { exit 1 }
