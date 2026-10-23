<#
.SYNOPSIS
  The release-gate outputs drill (#443): record + ISO + stream + registered OS
  virtual camera against an already-installed CoreVideo Pro, judged per frame.

.DESCRIPTION
  Owner ruling 2026-10-22: outputs are the release gate. Program recording, every
  ISO, the registered OS virtual camera and the stream must deliver every frame
  exactly once over the run. On-screen Studio cells are NOT judged here.

  Turned from the one-off harness that passed the 2026-10-22 30-minute soak
  (artifacts/qualification-1008-25: Launch-InstalledShortcut, Prepare-LiveWorkload,
  Start-LocalSrtSink, Wait-Ready-Refreshed, Collect-Identity,
  Finalize-DeclaredInterval). The judging logic is the existing scripts/qa
  modules, unchanged; see outputs-drill-judge.py and docs/qa/outputs-drill.md.

  Safety: refuses if the app is already running; drives the app only through the
  control API on 127.0.0.1:8011; never enables RTMP and never reads or writes the
  stored RTMP key by value; streams only to a loopback SRT sink it starts itself;
  no SendKeys, no elevation, no PresentMon. The preferences file is restored from
  a byte-exact backup at the end, including on failure.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.ps1 -Seconds 1800
#>
param(
  [string]$InstallRoot,
  [int]$Seconds = 1800,
  [string]$Out = (Join-Path 'D:\CoreVideo-QA\drills' (Get-Date -Format 'yyyyMMdd-HHmmss')),
  [string[]]$RemoteIds = @(),
  [string]$MeetingUrl,
  [string]$WebcamSourceId,
  [string]$CameraReceiver,
  [switch]$SkipStream,
  [switch]$SkipVirtualCamera
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$api = 'http://127.0.0.1:8011'

function Invoke-DrillAction([string]$Action, [object[]]$Arguments = @()) {
  $body = @{ action = $Action; args = $Arguments } | ConvertTo-Json -Depth 8 -Compress
  Invoke-RestMethod "$api/invoke" -Method Post -ContentType application/json -Body $body -TimeoutSec 30 | Out-Null
}
function Get-DrillEnvelope { Invoke-RestMethod "$api/snapshot" -TimeoutSec 5 }
function Write-DrillJson($Value, [string]$Name, [int]$Depth = 8) {
  $Value | ConvertTo-Json -Depth $Depth | Set-Content -LiteralPath (Join-Path $Out $Name) -Encoding UTF8
}
function Write-DrillLog([string]$Message) {
  $line = '[' + [DateTime]::UtcNow.ToString('o') + '] ' + $Message
  Write-Host $line
  Add-Content -LiteralPath (Join-Path $Out 'drill.log') -Value $line -Encoding UTF8
}

# ---------------------------------------------------------------- preflight
$RemoteIds = ConvertTo-DrillIdList $RemoteIds   # -File passes '1,2,3' as one string
$problems = Test-DrillParameters -Seconds $Seconds -Out $Out -RemoteIds $RemoteIds -MeetingUrl $MeetingUrl -WebcamSourceId $WebcamSourceId
if ($problems.Count) { throw ('Invalid parameters: ' + ($problems -join ' ')) }
if (Get-Process CoreVideoPro.WinUI -ErrorAction SilentlyContinue) { throw 'CoreVideo Pro is already running; close it first (the drill must own the launch).' }
$listening = $false; try { $null = Invoke-RestMethod "$api/snapshot" -TimeoutSec 2; $listening = $true } catch { }
if ($listening) { throw 'Something already answers on 127.0.0.1:8011.' }
if (-not $InstallRoot) { $InstallRoot = Resolve-DrillInstallRoot }
$InstallRoot = (Resolve-Path -LiteralPath $InstallRoot).Path
$launcher = Join-Path $InstallRoot 'StartCoreVideo.cmd'
$shellExe = Join-Path $InstallRoot 'CoreVideoPro.WinUI.exe'
foreach ($required in $launcher, $shellExe) { if (-not (Test-Path -LiteralPath $required)) { throw "Not an installed app folder (missing $required)" } }
$python = (Get-Command python -ErrorAction Stop).Source
$node = (Get-Command node -ErrorAction Stop).Source
$ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source
$decodeFfmpeg = if (Test-Path (Join-Path $InstallRoot 'ffmpeg.exe')) { Join-Path $InstallRoot 'ffmpeg.exe' } else { (Get-Command ffmpeg -ErrorAction Stop).Source }
$sinkFfmpeg = $null
if (-not $SkipStream) {
  foreach ($candidate in @((Get-Command ffmpeg -ErrorAction SilentlyContinue).Source, (Join-Path $InstallRoot 'ffmpeg.exe'))) {
    if ($candidate -and (Test-Path -LiteralPath $candidate) -and ((& $candidate -hide_banner -protocols 2>$null) -match '^\s*srt$')) { $sinkFfmpeg = $candidate; break }
  }
  if (-not $sinkFfmpeg) { throw 'No ffmpeg with the srt protocol for the local sink; install one on PATH or pass -SkipStream.' }
}
if (-not $SkipVirtualCamera) {
  if (-not $CameraReceiver) { $CameraReceiver = Join-Path $repoRoot 'native\build-dev\corevideo-shared-camera-receiver.exe' }
  if (-not (Test-Path -LiteralPath $CameraReceiver)) {
    throw "OS camera receiver not found at $CameraReceiver. Build target corevideo-shared-camera-receiver (native/CMakeLists.txt) and pass -CameraReceiver, or pass -SkipVirtualCamera."
  }
  $CameraReceiver = (Resolve-Path -LiteralPath $CameraReceiver).Path
}
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) { throw "Evidence folder is not empty: $Out" }
$null = New-Item -ItemType Directory -Path $Out -Force
$Out = (Resolve-Path -LiteralPath $Out).Path
$prefsPath = Join-Path $env:LOCALAPPDATA 'CoreVideoPro\production-output-preferences.json'
$prefsBackup = Join-Path $Out 'preferences-backup.json'
$originalPrefs = Get-Content -LiteralPath $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $WebcamSourceId) {
  $known = @(Get-DrillWebcamFromPreferences $originalPrefs)
  if ($known.Count -ne 1) { throw ("Cannot pick the webcam from the profile (found: " + ($known -join ', ') + '). Pass -WebcamSourceId capture:<id>.') }
  $WebcamSourceId = $known[0]
}
$plan = Get-DrillWorkloadPlan -RemoteIds $RemoteIds -WebcamSourceId $WebcamSourceId -Stream (-not $SkipStream) -Camera (-not $SkipVirtualCamera)
$notes = @()
if ($plan.LocalOnly) { $notes += 'Local sources only: no remote Zoom feeds were requested (-RemoteIds), so no Zoom ISO was judged.' }
$notes += 'Screen capture is in Program but has no QA ISO identity stamp, so it is not ISO-recorded.'
$drill = [ordered]@{
  schema = 'outputs-drill-v1'; seconds = $Seconds; installRoot = $InstallRoot; workload = $plan.Workload
  remoteIds = $plan.RemoteIds; webcamSourceId = $WebcamSourceId; programSourceIds = $plan.ProgramSourceIds
  outputs = [ordered]@{ program = $true; iso = $plan.IsoSourceIds; stream = $plan.Stream; camera = $plan.Camera }
  tools = [ordered]@{ python = $python; node = $node; ffmpeg = $decodeFfmpeg; ffprobe = $ffprobe; sinkFfmpeg = $sinkFfmpeg; cameraReceiver = $CameraReceiver }
  notes = $notes; startedUtc = [DateTime]::UtcNow.ToString('o')
  operator = [Security.Principal.WindowsIdentity]::GetCurrent().Name
}
Write-DrillJson $drill 'drill.json'
Write-DrillLog "Outputs drill: $Seconds s, $($plan.Workload); evidence $Out"

# ---------------------------------------------------------------- live run
$prefsHash = Backup-DrillPreferences -Path $prefsPath -BackupPath $prefsBackup
$savedEnv = @{}
$envNames = 'COREVIDEO_QA_ISO_COUNTER', 'COREVIDEO_QA_PROGRAM_COUNTER', 'COREVIDEO_DELIVERY_TRACE', 'COREVIDEO_DELIVERY_TRACE_STAGES',
  'COREVIDEO_DELIVERY_TRACE_PATH', 'COREVIDEO_GPU_CAPTURE', 'COREVIDEO_PREPARE_PROGRAM_WITHIN_BUDGET', 'COREVIDEO_NATIVE_MONITOR_DLL_PATH',
  'COREVIDEO_ZOOM_ENGINE_PATH', 'COREVIDEO_FFMPEG_BIN_DIR', 'COREVIDEO_MONITOR_DIAGNOSTICS'
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$self = [Diagnostics.Process]::GetCurrentProcess()
$savedPriority = $self.PriorityClass
$app = $null; $sink = $null; $receiver = $null
Add-Type 'using System;using System.Runtime.InteropServices; public static class DrillPowerRequest { [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags); }'
try {
  $srtPort = if ($plan.Stream) { Get-DrillFreeUdpPort } else { 0 }
  $patched = Set-DrillPreferences -Path $prefsPath -RecordingFolder (Join-Path $Out 'recordings') -IsoSourceIds $plan.IsoSourceIds -SrtPort $srtPort -Stream:$plan.Stream
  Write-DrillLog "Preferences patched (recording -> evidence folder, RTMP off$(if ($plan.Stream) { ", SRT caller -> 127.0.0.1:$srtPort" }))"

  # Launch through StartCoreVideo.cmd. Children inherit IDLE/BELOW_NORMAL from
  # the caller, so the launcher runs at Normal for the duration of the launch.
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
  $env:COREVIDEO_QA_ISO_COUNTER = '1'; $env:COREVIDEO_QA_PROGRAM_COUNTER = '1'
  $env:COREVIDEO_DELIVERY_TRACE = '1'; $env:COREVIDEO_DELIVERY_TRACE_STAGES = 'all'
  $env:COREVIDEO_DELIVERY_TRACE_PATH = Join-Path $Out 'arrival-trace.bin'   # fixed at launch
  $self.PriorityClass = 'Normal'
  $null = Start-Process -FilePath $launcher -WorkingDirectory $InstallRoot -WindowStyle Normal -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(90)
  do {
    $found = @(Get-Process CoreVideoPro.WinUI -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $shellExe })
    if ($found.Count -eq 1) { $app = $found[0]; break }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'StartCoreVideo.cmd did not start the installed app' }
    Start-Sleep -Milliseconds 300
  } while ($true)
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
  $self.PriorityClass = $savedPriority
  if ($app.PriorityClass -ne 'Normal') { throw "Installed app is not at Normal priority ($($app.PriorityClass))" }
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  Write-DrillJson ([ordered]@{ appPid = $app.Id; shellPath = $app.Path; launcher = $launcher; priorityClass = $app.PriorityClass.ToString()
      elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator); qaCounters = $true; traceSelection = 'all'
      nativeSha256 = (Get-FileHash (Join-Path $InstallRoot 'corevideo-native.exe')).Hash
      cameraSha256 = $(if (Test-Path (Join-Path $InstallRoot 'corevideo-virtualcam.dll')) { (Get-FileHash (Join-Path $InstallRoot 'corevideo-virtualcam.dll')).Hash } else { $null })
      startedUtc = [DateTime]::UtcNow.ToString('o') }) 'launch.json'
  Write-DrillLog "App started (pid $($app.Id))"

  # Workload (Prepare-LiveWorkload.ps1).
  $deadline = [DateTime]::UtcNow.AddSeconds(90)
  do {
    try { $e = Get-DrillEnvelope; if ($e.available -and $e.ageMs -lt 1000) { break } } catch { }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Control API did not become ready' }
    Start-Sleep -Milliseconds 300
  } while ($true)
  if ($plan.RemoteIds.Count -and $e.snapshot.meetingState -ne 'in_meeting') { Invoke-DrillAction 'zoom.join' @($MeetingUrl); Write-DrillLog 'Joining the meeting' }
  Invoke-DrillAction 'automation.autoTake.set' @($false)
  Invoke-DrillAction 'automation.autoAssignInputs.set' @($false)
  Invoke-DrillAction 'transport.engine.set' @($true)
  foreach ($a in $plan.Assignments) { Invoke-DrillAction 'input.assign' @($a.slot, $a.source) }
  Invoke-DrillAction 'scene.cueWithRoutes' @($plan.Scene, $plan.Routes)
  $deadline = [DateTime]::UtcNow.AddSeconds(45)
  do {
    $state = Invoke-RestMethod "$api/state" -TimeoutSec 5
    $routed = (@($state.nativeProgramVideoSources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
    $alreadyLive = $state.nativeActiveSceneId -eq $plan.Scene -and $routed -eq ($plan.ProgramSourceIds -join '|')
    if ($state.canTake -or $alreadyLive) { break }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Cue did not become ready to Take' }
    Start-Sleep -Milliseconds 200
  } while ($true)
  if (-not $alreadyLive) { Invoke-DrillAction 'transport.take' @() }
  Write-DrillLog 'Program routed; starting outputs'

  # Local SRT sink (Start-LocalSrtSink.ps1), then record + stream.
  if ($plan.Stream) {
    $received = Join-Path $Out 'stream-received.ts'
    $passphrase = if ($patched.PSObject.Properties['StreamSrtPassphrase']) { [string]$patched.StreamSrtPassphrase } else { '' }
    $keyLength = 0; if ($patched.PSObject.Properties['StreamSrtKeyLength']) { [void][int]::TryParse([string]$patched.StreamSrtKeyLength, [ref]$keyLength) }
    $latency = if ($patched.PSObject.Properties['StreamSrtLatencyMs']) { [int]$patched.StreamSrtLatencyMs } else { 600 }
    $url = New-DrillSrtSinkUrl -Port $srtPort -LatencyMs $latency -ListenTimeoutSeconds ($Seconds + 900) -Passphrase $passphrase -KeyLength $keyLength
    $sinkErr = Join-Path $Out 'srt-sink.err.log'
    $sink = Start-Process -FilePath $sinkFfmpeg -ArgumentList @('-hide_banner', '-loglevel', 'warning', '-nostdin', '-y', '-i', ('"' + $url + '"'), '-c', 'copy', '-f', 'mpegts', ('"' + $received + '"')) `
      -RedirectStandardError $sinkErr -WindowStyle Hidden -PassThru
    $null = $sink.Handle
    Start-Sleep -Seconds 3
    if ($sink.HasExited) { throw ('SRT sink exited before the stream started: ' + (Get-Content $sinkErr -Tail 1)) }
    # Never write the passphrase into evidence.
    Write-DrillJson ([ordered]@{ pid = $sink.Id; port = $srtPort; latencyMs = $latency; encrypted = [bool]$passphrase; received = $received; startedUtc = [DateTime]::UtcNow.ToString('o') }) 'srt-sink.json'
  }
  Invoke-DrillAction 'transport.record.set' @($true)
  if ($plan.Stream) { Invoke-DrillAction 'transport.stream.set' @($true) }

  # Readiness (Wait-Ready-Refreshed.ps1): every check true for 30 s, max 300 s.
  $warm = [IO.StreamWriter]::new((Join-Path $Out 'warmup.ndjson'), $false, [Text.UTF8Encoding]::new($false))
  $clock = [Diagnostics.Stopwatch]::StartNew(); $stable = $null
  try {
    do {
      $e = Get-DrillEnvelope
      $r = Test-DrillReadiness -Envelope $e -Plan $plan
      $warm.WriteLine((@{ utc = [DateTime]::UtcNow.ToString('o'); ready = $r.Ready; reasons = $r.Reasons } | ConvertTo-Json -Compress)); $warm.Flush()
      if ($r.Fatal) { throw $r.Fatal }
      if ($plan.Stream -and $sink.HasExited) { throw 'SRT sink died after stream start' }
      if ($r.Ready) { if ($null -eq $stable) { $stable = $clock.Elapsed.TotalSeconds }; if ($clock.Elapsed.TotalSeconds - $stable -ge 30) { break } } else { $stable = $null }
      if ($clock.Elapsed.TotalSeconds -gt 300) { throw ('Outputs did not stabilize: ' + ($r.Reasons -join '; ')) }
      Start-Sleep -Seconds 1
    } while ($true)
  } finally { $warm.Dispose() }
  Write-DrillJson @{ utc = [DateTime]::UtcNow.ToString('o'); stableSeconds = 30; snapshot = $e.snapshot } 'recording-ready.json' 64
  Write-DrillLog 'Outputs ready (30 s stable)'

  # Collection (Collect-Identity.ps1).
  $null = [DrillPowerRequest]::SetThreadExecutionState(2147483651)
  $expectedEpoch = $e.snapshot.graphics.coreEpoch
  $receiverDir = Join-Path $Out 'os-camera'
  $receiverSeconds = 45 + $Seconds + 55
  if ($plan.Camera) {
    $null = New-Item -ItemType Directory -Path $receiverDir
    Invoke-DrillAction 'transport.virtualcam.set' @($true)
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
      $e = Get-DrillEnvelope
      $cameras = @($e.snapshot.captureDevices | Where-Object { $_.name -like 'CoreVideo Pro Camera*' })
      if ($e.available -and $cameras.Count -eq 1 -and $cameras[0].PSObject.Properties['nativeDeviceId'] -and $cameras[0].nativeDeviceId) { break }
      if ([DateTime]::UtcNow -gt $deadline) { throw 'Registered Windows camera did not appear in device discovery within 20 s' }
      Start-Sleep -Milliseconds 300
    } while ($true)
    $device = $cameras[0].nativeDeviceId
    Write-DrillJson ([ordered]@{ startedUtc = [DateTime]::UtcNow.ToString('o'); deviceId = $device; requestedSeconds = $receiverSeconds; receiverSha256 = (Get-FileHash $CameraReceiver).Hash }) 'os-camera\identity.json'
    $receiver = Start-Process -FilePath $CameraReceiver -ArgumentList @(('"' + $device + '"'), [string]$receiverSeconds) -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput (Join-Path $receiverDir 'receiver.ndjson') -RedirectStandardError (Join-Path $receiverDir 'receiver.stderr.log')
    $null = $receiver.Handle
    # First frame, then prove the registered source serves the installed DLL.
    $launchInfo = Get-Content (Join-Path $Out 'launch.json') -Raw | ConvertFrom-Json
    $expectedDll = if ($launchInfo.cameraSha256) { 'C:\Program Files\Common Files\CoreVideoProCamera\' + $launchInfo.cameraSha256.ToLowerInvariant() + '\corevideo-virtualcam.dll' } else { $null }
    $deadline = [DateTime]::UtcNow.AddSeconds(35); $firstFrame = $false
    do {
      if ($receiver.HasExited) { throw "OS camera receiver exited before capture (exit $($receiver.ExitCode))" }
      $stderr = Join-Path $receiverDir 'receiver.stderr.log'
      if (-not $firstFrame -and (Test-Path $stderr) -and (Get-Content $stderr -Raw) -match '\[receiver\] first-frame identity=[0-9]+') {
        $header = Get-Content (Join-Path $receiverDir 'receiver.ndjson') -TotalCount 1 | ConvertFrom-Json
        if ($header.receiverMode -ne 'os-camera' -or $header.width -ne 1920 -or $header.height -ne 1080 -or $header.fpsNumerator -ne 60 -or $header.fpsDenominator -ne 1) { throw 'OS receiver negotiated the wrong format/cadence' }
        $firstFrame = $true
      }
      if ($firstFrame -and $expectedDll) {
        $serve = @(Get-Content 'C:\ProgramData\CoreVideoPro\vcam-serve.log' -Tail 3000 -ErrorAction SilentlyContinue)
        $module = @($serve | Where-Object { $_ -like ('*Serving module path=' + $expectedDll) }) | Select-Object -Last 1
        if ($module -and $module -match 'pid=(\d+)') {
          $history = @($serve | Where-Object { $_ -match ('pid=' + $Matches[1] + '\] .*\[vcam-delivery-v2\]') -and $_ -match 'historyReads=[1-9][0-9]*' }) | Select-Object -Last 1
          if ($history) { Write-DrillJson ([ordered]@{ expectedDll = $expectedDll; moduleLine = $module; historyLine = $history; observedUtc = [DateTime]::UtcNow.ToString('o') }) 'camera-serving-identity.json'; break }
        }
      }
      if ([DateTime]::UtcNow -gt $deadline) {
        if (-not $firstFrame) { throw 'OS camera receiver produced no first frame within 35 s' }
        Write-DrillJson ([ordered]@{ expectedDll = $expectedDll; error = 'Registered OS source did not prove the expected DLL and active retained reads'; utc = [DateTime]::UtcNow.ToString('o') }) 'camera-serving-identity-failure.json'
        Write-DrillLog 'WARNING: camera serving module not proven; the OS camera will be NOT JUDGED'
        break
      }
      Start-Sleep -Milliseconds 250
    } while ($true)
    Write-DrillLog 'OS camera receiver capturing'
  }

  $samples = [IO.StreamWriter]::new((Join-Path $Out 'samples.ndjson'), $false, [Text.UTF8Encoding]::new($false))
  $clock = [Diagnostics.Stopwatch]::StartNew(); $start = $null; $end = $null; $startElapsed = $null
  try {
    do {
      $e = Get-DrillEnvelope
      $broken = Test-DrillInvariant -Envelope $e -Plan $plan -ExpectedEpoch $expectedEpoch
      if ($broken) { throw $broken }
      if ($plan.Stream -and $sink.HasExited) { throw 'SRT sink exited during measurement' }
      if ($plan.Camera -and $receiver.HasExited -and -not $end) { throw "OS camera receiver exited during measurement (exit $($receiver.ExitCode))" }
      $row = Get-DrillSampleRow -Envelope $e -ElapsedMs $clock.Elapsed.TotalMilliseconds
      $samples.WriteLine(($row | ConvertTo-Json -Depth 8 -Compress)); $samples.Flush()
      if ($null -eq $start -and $clock.Elapsed.TotalSeconds -ge 45) { $start = @{ utc = $row.utc; qpc = $row.qpc }; $startElapsed = $clock.Elapsed.TotalSeconds; Write-DrillLog 'Measured interval started' }
      if ($null -eq $end -and $null -ne $startElapsed -and $clock.Elapsed.TotalSeconds -ge $startElapsed + $Seconds) {
        $end = @{ utc = $row.utc; qpc = $row.qpc }
        Write-DrillJson ([ordered]@{ start = $start; end = $end; qpcFrequency = [Diagnostics.Stopwatch]::Frequency; minimumSeconds = $Seconds }) 'measurement.json'
        Write-DrillLog 'Measured interval complete'
      }
      if ($end -and (-not $plan.Camera -or $receiver.HasExited)) { break }
      if ($clock.Elapsed.TotalSeconds -gt $receiverSeconds + 60) { throw 'OS camera receiver did not complete normally' }
      Start-Sleep -Seconds 1
    } while ($true)
  } finally { $samples.Dispose() }
  if ($plan.Camera) {
    $receiver.WaitForExit(); $receiver.Refresh()
    Write-DrillJson ([ordered]@{ pid = $receiver.Id; exitCode = $receiver.ExitCode; completedUtc = [DateTime]::UtcNow.ToString('o') }) 'os-camera\result.json'
    $receiver = $null
  }

  # Stop in Collect-Identity order: stream, record (finalize), trace, camera, engine.
  $e.snapshot | ConvertTo-Json -Depth 64 -Compress | Set-Content -LiteralPath (Join-Path $Out 'pre-stop.json') -Encoding UTF8
  if ($plan.Stream) { Invoke-DrillAction 'transport.stream.set' @($false) }
  Invoke-DrillAction 'transport.record.set' @($false)
  $deadline = [DateTime]::UtcNow.AddSeconds(60)
  do { $s = (Get-DrillEnvelope).snapshot; if ($s.recording.lifecycle.finalized) { break }; if ([DateTime]::UtcNow -gt $deadline) { throw 'Recording finalization failed' }; Start-Sleep -Seconds 1 } while ($true)
  $stop = [Threading.EventWaitHandle]::OpenExisting($s.deliveryEvidence.stopEvent); $null = $stop.Set(); $stop.Dispose()
  if ($plan.Camera) { Invoke-DrillAction 'transport.virtualcam.set' @($false) }
  Invoke-DrillAction 'transport.engine.set' @($false)
  $deadline = [DateTime]::UtcNow.AddSeconds(20)
  do { $s = (Get-DrillEnvelope).snapshot; if ($s.deliveryEvidence.captureComplete) { break }; if ([DateTime]::UtcNow -gt $deadline) { throw 'Trace did not finalize' }; Start-Sleep -Seconds 1 } while ($true)
  $s | ConvertTo-Json -Depth 64 -Compress | Set-Content -LiteralPath (Join-Path $Out 'finalized.json') -Encoding UTF8
  Write-DrillLog 'Outputs stopped; recording and trace finalized'
} catch {
  $failure = $_.Exception.Message
  Write-DrillLog "COLLECTION FAILED: $failure"
  if (-not (Test-Path (Join-Path $Out 'collection-failure.json'))) {
    Write-DrillJson ([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); error = $failure; at = $_.InvocationInfo.PositionMessage }) 'collection-failure.json'
  }
  foreach ($action in 'transport.stream.set', 'transport.record.set', 'transport.virtualcam.set') { try { Invoke-DrillAction $action @($false) } catch { } }
} finally {
  $null = [DrillPowerRequest]::SetThreadExecutionState(2147483648)
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
  try { $self.PriorityClass = $savedPriority } catch { }
  if ($receiver -and -not $receiver.HasExited) {
    Write-DrillJson @{ utc = [DateTime]::UtcNow.ToString('o'); receiverPid = $receiver.Id; reason = 'Drill failed; receiver cancelled, not a completed capture' } 'os-camera\cancelled.json'
    Stop-Process -Id $receiver.Id -ErrorAction SilentlyContinue
  }
  if ($sink) {
    Start-Sleep -Seconds 5
    if (-not $sink.HasExited) { Stop-Process -Id $sink.Id -ErrorAction SilentlyContinue; $null = $sink.WaitForExit(10000) }
    $bytes = if (Test-Path (Join-Path $Out 'stream-received.ts')) { (Get-Item (Join-Path $Out 'stream-received.ts')).Length } else { 0 }
    Write-DrillJson @{ bytes = $bytes; stoppedUtc = [DateTime]::UtcNow.ToString('o') } 'srt-sink-stopped.json'
  }
  if ($app -and -not $app.HasExited) {
    try { $m = (Get-DrillEnvelope).snapshot.meetingState; if ($m -eq 'in_meeting') { Invoke-DrillAction 'zoom.leave' @(); Start-Sleep -Seconds 3 } } catch { }
    $null = $app.CloseMainWindow()   # WM_CLOSE, the harness's exit path
    if (-not $app.WaitForExit(45000)) { Write-DrillLog 'WARNING: app did not exit within 45 s; it may rewrite preferences when it does' }
  }
  try {
    $null = Restore-DrillPreferences -Path $prefsPath -BackupPath $prefsBackup -ExpectedHash $prefsHash
    Write-DrillLog 'Preferences restored byte-for-byte'
  } catch { Write-DrillLog "ERROR: preferences NOT restored: $($_.Exception.Message). Copy $prefsBackup back to $prefsPath." }
}

# ---------------------------------------------------------------- offline verdict
Write-DrillLog 'Judging outputs offline'
& $python (Join-Path $PSScriptRoot 'outputs-drill-judge.py') $Out
$judgeExit = $LASTEXITCODE
if (-not (Test-Path (Join-Path $Out 'verdict.json'))) { throw 'Judge did not write verdict.json' }
Write-DrillLog "Verdict: $((Get-Content (Join-Path $Out 'verdict.json') -Raw | ConvertFrom-Json).overall) (see summary.txt)"
exit $judgeExit
