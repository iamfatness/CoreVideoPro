# Pester-free tests for scripts/qa/graceful-close-check.ps1 (#457), pure parts only.
# Never finds, closes or queries the real app: the script is dot-sourced, which
# defines its functions and returns before any live step. Exits non-zero on any failure.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\graceful-close-check.test.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'graceful-close-check.ps1')

$script:failures = 0; $script:cases = 0
function Check([string]$Name, [scriptblock]$Body) {
  $script:cases++
  try { & $Body; Write-Host "PASS  $Name" }
  catch { $script:failures++; Write-Host "FAIL  $Name :: $($_.Exception.Message)" -ForegroundColor Red }
}
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

$stamp = 'build=1.0.0+1879ccb7aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa role=app]'
function L([string]$Time, [int]$ProcessId, [string]$Session, [string]$Text) { "[2026-10-23T$Time-04:00] [pid=$ProcessId session=$Session $stamp $Text" }

$cleanLog = @(
  (L '09:00:00.0000000' 4242 'aaaa1111' 'zoom-join: snapshot meetingState=in_meeting participants=3 warnings=0'),
  (L '09:00:01.0000000' 777 'bbbb2222' 'some other process'),
  (L '09:31:00.0000000' 4242 'aaaa1111' 'shutdown: close requested'),
  (L '09:31:00.3000000' 4242 'aaaa1111' 'shutdown: resources released'),
  (L '09:31:00.3100000' 4242 'aaaa1111' 'shutdown: cleanup complete; terminating the process directly (skips the WinUI dispatcher drain, T1.7)'),
  (L '09:31:02.0000000' 777 'bbbb2222' 'later line from another process')
)
# The 17304 shape: the drain fail-fast left "resources released" as the last line.
$drainLog = @(
  (L '09:31:00.0000000' 4242 'aaaa1111' 'shutdown: close requested'),
  (L '09:31:00.3000000' 4242 'aaaa1111' 'shutdown: resources released')
)
$fakeEvent = [pscustomobject]@{ time = '2026-10-23T09:31:01'; module = 'CoreMessagingXP.dll'; code = '0xc000027b'; offset = '0x0000000000097de6'; processId = '0x1092' }

# ---------------------------------------------------------------- log reading
Check 'session id comes from the last line of this pid' {
  Assert ((Get-GracefulCloseSession -Lines $cleanLog -ProcessId 4242) -eq 'aaaa1111') 'wrong session'
  Assert ($null -eq (Get-GracefulCloseSession -Lines $cleanLog -ProcessId 9999)) 'unknown pid gave a session'
}
Check 'last line is this session, not a later process' {
  $last = Get-GracefulCloseLastLine -Lines $cleanLog -Session 'aaaa1111'
  Assert ($last -match 'terminating the process directly') $last
}
Check 'a reused pid in a later session does not count' {
  $lines = $drainLog + @(L '10:00:00.0000000' 4242 'cccc3333' 'shutdown: cleanup complete; terminating the process directly')
  $last = Get-GracefulCloseLastLine -Lines $lines -Session 'aaaa1111'
  Assert ($last -match 'resources released') $last
}

# ---------------------------------------------------------------- verdict
Check 'drain shape + new event 1000 fails with both reasons' {
  $v = Get-GracefulCloseVerdict -LastLine (Get-GracefulCloseLastLine -Lines $drainLog -Session 'aaaa1111') -Exited $true -ExitMs 900 -NewDumps @() -NewEvents @($fakeEvent)
  Assert (-not $v.pass) 'passed'
  Assert (@($v.reasons | Where-Object { $_ -match 'terminating the process directly' }).Count -eq 1) ($v.reasons -join ';')
  Assert (@($v.reasons | Where-Object { $_ -match 'event 1000' }).Count -eq 1) ($v.reasons -join ';')
  Assert (@($v.newEvent1000).Count -eq 1) 'event not carried'
  Assert (-not $v.terminateLine) 'terminateLine true'
}
Check 'terminate line, exit within 6 s, nothing new passes' {
  $v = Get-GracefulCloseVerdict -LastLine (Get-GracefulCloseLastLine -Lines $cleanLog -Session 'aaaa1111') -Exited $true -ExitMs 1200 -NewDumps @() -NewEvents @()
  Assert $v.pass ($v.reasons -join ';')
  Assert $v.terminateLine 'terminateLine false'
  Assert ($v.exitMs -eq 1200) "exitMs $($v.exitMs)"
  Assert (@($v.reasons).Count -eq 0) ($v.reasons -join ';')
}
Check 'slow exit fails' {
  $v = Get-GracefulCloseVerdict -LastLine 'x terminating the process directly' -Exited $true -ExitMs 6001 -NewDumps @() -NewEvents @()
  Assert (-not $v.pass) 'passed'
  Assert (($v.reasons -join ';') -match '6000') ($v.reasons -join ';')
}
Check 'no exit fails' {
  $v = Get-GracefulCloseVerdict -LastLine 'x terminating the process directly' -Exited $false -ExitMs $null -NewDumps @() -NewEvents @()
  Assert (-not $v.pass) 'passed'
  Assert (($v.reasons -join ';') -match 'did not exit') ($v.reasons -join ';')
}
Check 'new dump fails' {
  $v = Get-GracefulCloseVerdict -LastLine 'x terminating the process directly' -Exited $true -ExitMs 100 -NewDumps @('C:\x\CoreVideoPro.WinUI.exe.4242.dmp') -NewEvents @()
  Assert (-not $v.pass) 'passed'
  Assert (@($v.newDumps).Count -eq 1) 'dump not carried'
}
Check 'the ForceExit fallback line fails' {
  $v = Get-GracefulCloseVerdict -LastLine 'x shutdown: force exit code=0' -Exited $true -ExitMs 100 -NewDumps @() -NewEvents @()
  Assert (-not $v.pass) 'passed'
}
Check 'missing log line fails' {
  $v = Get-GracefulCloseVerdict -LastLine $null -Exited $true -ExitMs 100 -NewDumps @() -NewEvents @()
  Assert (-not $v.pass) 'passed'
}

# ---------------------------------------------------------------- refusals
$idleState = [pscustomobject]@{ recording = $false; streaming = $false; engineOn = $true }
$inMeeting = [pscustomobject]@{ meetingState = 'in_meeting'; recording = [pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'idle' } }; outputSenderSession = [pscustomobject]@{ activeSenderCount = 0; senders = @() } }
$now = [datetime]'2026-10-23T10:00:00'
Check 'engine on, in meeting, 31 min up: no refusal (engine on is not a close-guard prompt)' {
  $r = @(Get-GracefulCloseRefusals -State $idleState -Snapshot $inMeeting -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30)
  Assert ($r.Count -eq 0) ($r -join ';')
}
Check 'recording or streaming refuses (the close guard would prompt)' {
  $rec = [pscustomobject]@{ recording = $true; streaming = $false; engineOn = $true }
  Assert (@(Get-GracefulCloseRefusals -State $rec -Snapshot $inMeeting -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30).Count -ge 1) 'recording accepted'
  $str = [pscustomobject]@{ recording = $false; streaming = $true; engineOn = $true }
  Assert (@(Get-GracefulCloseRefusals -State $str -Snapshot $inMeeting -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30).Count -ge 1) 'streaming accepted'
}
Check 'a finalizing recording refuses' {
  $fin = [pscustomobject]@{ meetingState = 'in_meeting'; recording = [pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'finalizing' } }; outputSenderSession = $null }
  Assert (@(Get-GracefulCloseRefusals -State $idleState -Snapshot $fin -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30).Count -ge 1) 'finalizing accepted'
}
Check 'a short session refuses unless the minimum is lowered' {
  Assert (@(Get-GracefulCloseRefusals -State $idleState -Snapshot $inMeeting -ProcessStart $now.AddMinutes(-10) -Now $now -MinSessionMinutes 30).Count -eq 1) 'short accepted'
  Assert (@(Get-GracefulCloseRefusals -State $idleState -Snapshot $inMeeting -ProcessStart $now.AddMinutes(-10) -Now $now -MinSessionMinutes 0).Count -eq 0) 'override ignored'
}
Check 'not in a meeting refuses unless allowed' {
  $out = [pscustomobject]@{ meetingState = 'idle'; recording = $null; outputSenderSession = $null }
  Assert (@(Get-GracefulCloseRefusals -State $idleState -Snapshot $out -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30).Count -eq 1) 'outside accepted'
  Assert (@(Get-GracefulCloseRefusals -State $idleState -Snapshot $out -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30 -AllowOutsideMeeting).Count -eq 0) 'allow ignored'
}
Check 'no control API answer refuses' {
  Assert (@(Get-GracefulCloseRefusals -State $null -Snapshot $null -ProcessStart $now.AddMinutes(-31) -Now $now -MinSessionMinutes 30).Count -ge 1) 'null accepted'
}
Check 'evidence folder must be absolute on D:' {
  Assert (@(Test-GracefulCloseOut 'D:\CoreVideo-QA\graceful-close\x').Count -eq 0) 'D: refused'
  Assert (@(Test-GracefulCloseOut 'C:\temp\x').Count -eq 1) 'C: accepted'
  Assert (@(Test-GracefulCloseOut 'rel\x').Count -eq 1) 'relative accepted'
}

Write-Host ''
Write-Host "$($script:cases - $script:failures)/$($script:cases) passed"
if ($script:failures) { exit 1 }
exit 0
