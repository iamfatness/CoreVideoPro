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

  Safety: refuses if the app or its core is already running; drives the app only
  through the control API on 127.0.0.1:8011; forces RTMP off and never changes
  the stored RTMP key or URL (the key is read only to prove the rewrite leaves it
  unchanged); refuses to start any output unless the app is running on the
  patched preferences; streams only to a loopback SRT sink it starts itself; no
  SendKeys, no elevation, no PresentMon. Outputs are stopped and finalized
  before WM_CLOSE (the shell's close guard would otherwise prompt and activate
  its window). The preferences file and its .bak sidecar are restored from
  byte-exact backups kept OUTSIDE the evidence folder, and only once the app has
  exited; otherwise preferences-restore-pending.json is written and the script
  exits 3.

  #935 (owner ruling D8): an ISO on the software encoder is a NOTE and its row
  passes on frame identity alone; Program on the software encoder stays a FAIL.
  -ProbeMaxSessions sets COREVIDEO_ENCODER_PROBE_MAX_SESSIONS for the launch
  (discovery depth only; it does not change probe semantics), which is how a
  software-ISO spill is forced on a GPU with spare sessions. Each second the
  collector also measures the core process CPU % and every software ISO's
  encode work per frame (measurement.json `encoderCost`, supplementary).

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
  [switch]$NoWebcam,
  [string]$MediaClip,
  [string]$CameraReceiver,
  [switch]$SkipStream,
  [switch]$SkipVirtualCamera,
  [int]$ProbeMaxSessions = 0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$api = 'http://127.0.0.1:8011'

function Invoke-DrillAction([string]$Action, [object[]]$Arguments = @()) {
  $body = @{ action = $Action; args = $Arguments } | ConvertTo-Json -Depth 8 -Compress
  try {
    Invoke-RestMethod "$api/invoke" -Method Post -ContentType application/json -Body $body -TimeoutSec 30 | Out-Null
  } catch {
    # The router answers 400 (bind) or 422 (the surface refused) with the reason
    # in the body; Invoke-RestMethod's own message drops it. Keep action + reason.
    $detail = $_.Exception.Message
    $response = $_.Exception.Response
    if ($response) {
      try { $reader = New-Object IO.StreamReader($response.GetResponseStream()); $text = $reader.ReadToEnd(); if ($text) { $detail = "HTTP $([int]$response.StatusCode): $text" } } catch { }
    }
    throw "$Action $($body) failed: $detail"
  }
}
function Get-DrillEnvelope { Invoke-RestMethod "$api/snapshot" -TimeoutSec 5 }
function Get-DrillState { Invoke-RestMethod "$api/state" -TimeoutSec 5 }
function Write-DrillJson($Value, [string]$Name, [int]$Depth = 8) {
  $Value | ConvertTo-Json -Depth $Depth | Set-Content -LiteralPath (Join-Path $Out $Name) -Encoding UTF8
}
function Write-DrillLog([string]$Message) {
  # The log is evidence, never a dependency: a reader holding drill.log open
  # (2026-10-28: a `tail -F` on it) made Add-Content throw a sharing violation
  # at "Outputs ready", and the throw inside `finally` left the app running on
  # patched preferences. Retry briefly, then keep going with the console line.
  $line = '[' + [DateTime]::UtcNow.ToString('o') + '] ' + $Message
  Write-Host $line
  for ($attempt = 0; $attempt -lt 20; $attempt++) {
    try { Add-Content -LiteralPath (Join-Path $Out 'drill.log') -Value $line -Encoding UTF8 -ErrorAction Stop; return }
    catch [System.IO.IOException] { Start-Sleep -Milliseconds 100 }
  }
  Write-Host "WARNING: drill.log could not be written (held open by another process?): $Message"
}
function Wait-DrillUntil([int]$Seconds, [scriptblock]$Condition) {
  $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do { try { if (& $Condition) { return $true } } catch { }; if ([DateTime]::UtcNow -gt $deadline) { return $false }; Start-Sleep -Milliseconds 500 } while ($true)
}

# ---------------------------------------------------------------- preflight
$RemoteIds = ConvertTo-DrillIdList $RemoteIds   # -File passes '1,2,3' as one string
$problems = Test-DrillParameters -Seconds $Seconds -Out $Out -RemoteIds $RemoteIds -MeetingUrl $MeetingUrl -WebcamSourceId $WebcamSourceId -ProbeMaxSessions $ProbeMaxSessions -NoWebcam ([bool]$NoWebcam) -MediaClip $MediaClip
if ($problems.Count) { throw ('Invalid parameters: ' + ($problems -join ' ')) }
$running = @(Get-Process CoreVideoPro.WinUI, corevideo-native, corevideo-zoom-engine -ErrorAction SilentlyContinue)
if ($running.Count) { throw ('CoreVideo Pro is already running (' + (($running | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') + '); close it first (the drill must own the launch).') }
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
$prefsPath = Join-Path $env:LOCALAPPDATA 'CoreVideoPro\production-output-preferences.json'
$originalPrefs = Get-Content -LiteralPath $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json
$sinkFfmpeg = $null
if (-not $SkipStream) {
  if (([string]$originalPrefs.StreamSrtPassphrase).StartsWith('dpapi:')) {
    throw 'The stored SRT passphrase is DPAPI-protected; the loopback sink cannot use it. Clear the SRT passphrase in Settings or pass -SkipStream.'
  }
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
$mediaSourceId = $null
if ($NoWebcam) {
  $WebcamSourceId = ''
  # The clip plays once: it must outlast settle + interval + receiver drain and
  # the 300 s readiness ceiling. Its bin id is a function of the staged path.
  $MediaClip = (Resolve-Path -LiteralPath $MediaClip).Path
  $clipProblems = @(Test-DrillMediaClip -ClipPath $MediaClip -Ffprobe $ffprobe -MinimumSeconds (45 + $Seconds + 55 + 300 + 60))
  if ($clipProblems.Count) { throw ("MediaClip $MediaClip is not usable: " + ($clipProblems -join '; ') + '. Make one with scripts\qa\outputs-drill-clip.ps1.') }
  $mediaSourceId = 'media:' + (Get-DrillMediaAssetId $script:DrillMediaRelativePath)
}
elseif (-not $WebcamSourceId) {
  $known = @(Get-DrillWebcamFromPreferences $originalPrefs)
  if ($known.Count -ne 1) { throw ("Cannot pick the webcam from the profile (found: " + ($known -join ', ') + '). Pass -WebcamSourceId capture:<id>.') }
  $WebcamSourceId = $known[0]
}
$plan = Get-DrillWorkloadPlan -RemoteIds $RemoteIds -WebcamSourceId $WebcamSourceId -MediaSourceId $mediaSourceId -Stream (-not $SkipStream) -Camera (-not $SkipVirtualCamera) -RecordingFolder (Join-Path $Out 'recordings')
$notes = @()
if ($plan.LocalOnly) { $notes += 'Local sources only: no remote Zoom feeds were requested (-RemoteIds), so no Zoom ISO was judged.' }
if (-not $plan.WebcamExercised) { $notes += "Webcam NOT EXERCISED (-NoWebcam): Program is the single-route scene '$($plan.Scene)' with the synthetic motion clip alone and no webcam ISO was recorded; this run proves nothing about UVC capture." }
if ($plan.MediaSourceId) { $notes += "Program source is a synthetic 1080p60 motion clip staged into the media bin as $($plan.MediaSourceId) from $MediaClip; it plays once (bin clips do not loop) and is removed after the run. No screen was captured." }
if ($plan.LocalOnly -and -not $plan.WebcamExercised) { $notes += 'No ISO in this run: a media clip carries no QA ISO identity.' }
if ($plan.CaptureSourceIds -contains $script:DrillScreenSourceId) { $notes += 'Screen capture is in Program but has no QA ISO identity stamp, so it is not ISO-recorded.' }
$workload = $plan.Workload
if ($ProbeMaxSessions -gt 0) {
  $workload += "; encoder probe limited to $ProbeMaxSessions sessions (COREVIDEO_ENCODER_PROBE_MAX_SESSIONS)"
  $notes += "Encoder capacity discovery was limited to $ProbeMaxSessions sessions for this run (COREVIDEO_ENCODER_PROBE_MAX_SESSIONS); ISO placement follows from that."
}
$backupDir = Join-Path $env:LOCALAPPDATA ('CoreVideoPro\outputs-drill-backups\' + (Split-Path -Leaf $Out) + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$drill = [ordered]@{
  schema = 'outputs-drill-v2'; seconds = $Seconds; installRoot = $InstallRoot; workload = $workload
  probeMaxSessions = $(if ($ProbeMaxSessions -gt 0) { $ProbeMaxSessions } else { $null })
  remoteIds = $plan.RemoteIds; webcamSourceId = $(if ($plan.WebcamExercised) { $WebcamSourceId } else { $null }); webcamExercised = $plan.WebcamExercised
  programSourceIds = $plan.ProgramSourceIds
  mediaClip = $(if ($plan.MediaSourceId) { $MediaClip } else { $null }); mediaSourceId = $plan.MediaSourceId
  outputs = [ordered]@{ program = $true; iso = @($plan.IsoSourceIds); stream = $plan.Stream; camera = $plan.Camera }
  tools = [ordered]@{ python = $python; node = $node; ffmpeg = $decodeFfmpeg; ffprobe = $ffprobe; sinkFfmpeg = $sinkFfmpeg; cameraReceiver = $CameraReceiver }
  preferencesBackupFolder = $backupDir   # path only; the copies stay outside the evidence folder
  notes = $notes; startedUtc = [DateTime]::UtcNow.ToString('o')
  operator = [Security.Principal.WindowsIdentity]::GetCurrent().Name
}
Write-DrillJson $drill 'drill.json'
Write-DrillLog "Outputs drill: $Seconds s, $workload; evidence $Out"

# ---------------------------------------------------------------- live run
# The show-input roster persists the drill's input.assign calls (2026-10-30: a
# drill's screen slot was left in the owner's roster), so it is restored too.
$rosterPath = Join-Path $env:LOCALAPPDATA 'CoreVideoPro\show-input-roster.json'
$manifest = Backup-DrillPreferenceFiles -Path $prefsPath -AlsoPaths @($rosterPath) -BackupDirectory $backupDir
Write-DrillLog "Preferences, .bak sidecar and show-input roster backed up to $backupDir"
$savedEnv = @{}
$envNames = 'COREVIDEO_QA_ISO_COUNTER', 'COREVIDEO_QA_PROGRAM_COUNTER', 'COREVIDEO_DELIVERY_TRACE', 'COREVIDEO_DELIVERY_TRACE_STAGES',
  'COREVIDEO_DELIVERY_TRACE_PATH', 'COREVIDEO_GPU_CAPTURE', 'COREVIDEO_PREPARE_PROGRAM_WITHIN_BUDGET', 'COREVIDEO_NATIVE_MONITOR_DLL_PATH',
  'COREVIDEO_ZOOM_ENGINE_PATH', 'COREVIDEO_FFMPEG_BIN_DIR', 'COREVIDEO_MONITOR_DIAGNOSTICS', 'COREVIDEO_ENCODER_PROBE_MAX_SESSIONS'
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$self = [Diagnostics.Process]::GetCurrentProcess()
$savedPriority = $self.PriorityClass
$app = $null; $sink = $null; $receiver = $null; $stagedClip = $null
$measured = $false; $failure = $null; $restorePending = $false; $safe = $false
Add-Type 'using System;using System.Runtime.InteropServices; public static class DrillPowerRequest { [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint flags); }'
try {
  # Children inherit IDLE/BELOW_NORMAL from their parent, so the drill runs at
  # Normal for the whole live phase: app, SRT sink and camera receiver all
  # start at Normal (the reference ran the receiver from a Normal task).
  $self.PriorityClass = 'Normal'

  $srtPort = 0
  if ($plan.Stream) { $srtPort = Get-DrillFreeUdpPort }
  $patch = Set-DrillPreferences -Path $prefsPath -RecordingFolder $plan.RecordingFolder -IsoSourceIds $plan.IsoSourceIds -SrtPort $srtPort -Stream:$plan.Stream
  Write-DrillLog "Preferences patched (recording -> evidence folder, RTMP off$(if ($plan.Stream) { ", SRT caller -> 127.0.0.1:$srtPort" }))"
  if ($plan.MediaSourceId) {
    $stagedClip = Install-DrillMediaClip -ClipPath $MediaClip -Root (Get-DrillMediaBinRoot)
    Write-DrillLog "Motion clip staged into the media bin as $($plan.MediaSourceId) ($stagedClip); the app scans the bin at startup"
  }

  # Local SRT sink (Start-LocalSrtSink.ps1), bound right after the port is chosen.
  if ($plan.Stream) {
    $received = Join-Path $Out 'stream-received.ts'
    $p = $patch.Preferences
    $passphrase = if ($p.PSObject.Properties['StreamSrtPassphrase']) { [string]$p.StreamSrtPassphrase } else { '' }
    $keyLength = 0; if ($p.PSObject.Properties['StreamSrtKeyLength']) { [void][int]::TryParse([string]$p.StreamSrtKeyLength, [ref]$keyLength) }
    $latency = 600; if ($p.PSObject.Properties['StreamSrtLatencyMs']) { [void][int]::TryParse([string]$p.StreamSrtLatencyMs, [ref]$latency) }
    $url = New-DrillSrtSinkUrl -Port $srtPort -LatencyMs $latency -ListenTimeoutSeconds ($Seconds + 1200) -Passphrase $passphrase -KeyLength $keyLength
    $sinkErr = Join-Path $Out 'srt-sink.err.log'
    $sink = Start-Process -FilePath $sinkFfmpeg -ArgumentList @('-hide_banner', '-loglevel', 'warning', '-nostdin', '-y', '-i', ('"' + $url + '"'), '-c', 'copy', '-f', 'mpegts', ('"' + $received + '"')) `
      -RedirectStandardError $sinkErr -WindowStyle Hidden -PassThru
    $null = $sink.Handle
    Start-Sleep -Seconds 2
    if ($sink.HasExited) { throw ('SRT sink exited before the stream started: ' + (Get-Content $sinkErr -Tail 1)) }
    if ($sink.PriorityClass -ne 'Normal') { throw "SRT sink is not at Normal priority ($($sink.PriorityClass))" }
    # Never write the passphrase into evidence.
    Write-DrillJson ([ordered]@{ pid = $sink.Id; port = $srtPort; latencyMs = $latency; encrypted = [bool]$passphrase; priorityClass = $sink.PriorityClass.ToString(); received = $received; startedUtc = [DateTime]::UtcNow.ToString('o') }) 'srt-sink.json'
  }

  # Launch through StartCoreVideo.cmd with the QA counters and the trace path (fixed at launch).
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
  $env:COREVIDEO_QA_ISO_COUNTER = '1'; $env:COREVIDEO_QA_PROGRAM_COUNTER = '1'
  $env:COREVIDEO_DELIVERY_TRACE = '1'; $env:COREVIDEO_DELIVERY_TRACE_STAGES = 'all'
  $env:COREVIDEO_DELIVERY_TRACE_PATH = Join-Path $Out 'arrival-trace.bin'
  if ($ProbeMaxSessions -gt 0) { $env:COREVIDEO_ENCODER_PROBE_MAX_SESSIONS = [string]$ProbeMaxSessions }
  $null = Start-Process -FilePath $launcher -WorkingDirectory $InstallRoot -WindowStyle Normal -PassThru
  $deadline = [DateTime]::UtcNow.AddSeconds(90)
  do {
    $found = @(Get-Process CoreVideoPro.WinUI -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $shellExe })
    if ($found.Count -eq 1) { $app = $found[0]; break }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'StartCoreVideo.cmd did not start the installed app' }
    Start-Sleep -Milliseconds 300
  } while ($true)
  $null = $app.Handle
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
  if ($app.PriorityClass -ne 'Normal') { throw "Installed app is not at Normal priority ($($app.PriorityClass))" }
  $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
  Write-DrillJson ([ordered]@{ appPid = $app.Id; shellPath = $app.Path; launcher = $launcher; priorityClass = $app.PriorityClass.ToString()
      elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator); qaCounters = $true; traceSelection = 'all'
      nativeSha256 = (Get-FileHash (Join-Path $InstallRoot 'corevideo-native.exe')).Hash
      cameraSha256 = $(if (Test-Path (Join-Path $InstallRoot 'corevideo-virtualcam.dll')) { (Get-FileHash (Join-Path $InstallRoot 'corevideo-virtualcam.dll')).Hash } else { $null })
      startedUtc = [DateTime]::UtcNow.ToString('o') }) 'launch.json'
  Write-DrillLog "App started (pid $($app.Id))"

  $e = $null
  if (-not (Wait-DrillUntil 90 { $script:e = Get-DrillEnvelope; $script:e.available -and $script:e.ageMs -lt 1000 })) { throw 'Control API did not become ready' }
  # The app has loaded its profile by now. A rejected patch would have been
  # replaced by the .bak recovery; refuse to start any output in that case.
  $notApplied = @(Test-DrillPreferencesApplied -Path $prefsPath -Expected $patch.Expected)
  Write-DrillJson ([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); applied = ($notApplied.Count -eq 0); problems = $notApplied }) 'preferences-applied.json'
  if ($notApplied.Count) { throw ('The app is not running on the patched preferences; no output started: ' + ($notApplied -join '; ')) }
  # UVC discovery can return nothing at startup until the adapter's 2 s refresh,
  # so settle for up to 10 s before refusing (a false refusal of a gate run).
  $webcamMissing = @()
  if ($plan.WebcamExercised) { $null = Wait-DrillUntil 10 { $script:webcamMissing = @(Test-DrillWebcamPresent -WebcamSourceId $WebcamSourceId -Devices @((Get-DrillEnvelope).snapshot.captureDevices)); $script:webcamMissing.Count -eq 0 } }
  else { Write-DrillLog "Webcam not exercised (-NoWebcam): no device check; Program scene '$($plan.Scene)' with the motion clip alone" }
  if ($webcamMissing.Count) { throw ($webcamMissing -join '; ') }

  # Workload (Prepare-LiveWorkload.ps1).
  if ($plan.RemoteIds.Count -and $e.snapshot.meetingState -ne 'in_meeting') { Invoke-DrillAction 'zoom.join' @($MeetingUrl); Write-DrillLog 'Joining the meeting' }
  Invoke-DrillAction 'automation.autoTake.set' @($false)
  Invoke-DrillAction 'automation.autoAssignInputs.set' @($false)
  if ($plan.EngineRequired) { Invoke-DrillAction 'transport.engine.set' @($true) }
  else { Write-DrillLog 'Local sources only: the Zoom capture engine stays off (transport.engine.set is refused outside a meeting)' }
  foreach ($a in $plan.Assignments) { Invoke-DrillAction 'input.assign' @($a.slot, $a.source) }
  Invoke-DrillAction 'scene.cueWithRoutes' @($plan.Scene, $plan.Routes)
  $deadline = [DateTime]::UtcNow.AddSeconds(45)
  do {
    $state = Get-DrillState
    $routed = (@($state.nativeProgramVideoSources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
    $alreadyLive = $state.nativeActiveSceneId -eq $plan.Scene -and $routed -eq ($plan.ProgramSourceIds -join '|')
    if ($state.canTake -or $alreadyLive) { break }
    if ([DateTime]::UtcNow -gt $deadline) { throw 'Cue did not become ready to Take' }
    Start-Sleep -Milliseconds 200
  } while ($true)
  if (-not $alreadyLive) { Invoke-DrillAction 'transport.take' @() }
  Write-DrillLog 'Program routed; starting outputs'
  Invoke-DrillAction 'transport.record.set' @($true)
  if ($plan.Stream) { Invoke-DrillAction 'transport.stream.set' @($true) }

  # Readiness (Wait-Ready-Refreshed.ps1 + Run-PictureDiagnostic.ps1): every check true for 30 s, max 300 s.
  $warm = [IO.StreamWriter]::new((Join-Path $Out 'warmup.ndjson'), $false, [Text.UTF8Encoding]::new($false))
  $clock = [Diagnostics.Stopwatch]::StartNew(); $stable = $null
  try {
    do {
      $e = Get-DrillEnvelope
      $r = Test-DrillReadiness -Envelope $e -Plan $plan
      $warm.WriteLine((@{ utc = [DateTime]::UtcNow.ToString('o'); ready = $r.Ready; reasons = $r.Reasons } | ConvertTo-Json -Compress)); $warm.Flush()
      if ($r.Fatal) { throw $r.Fatal }
      if ($plan.Stream -and $sink.HasExited) { throw 'SRT sink died after stream start' }
      if ($r.Ready -and $plan.Stream -and -not ((Test-Path $received) -and (Get-Item $received).Length -gt 0)) { $r = [pscustomobject]@{ Ready = $false; Reasons = @('loopback sink has received nothing yet') } }
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
    $receiver = Start-Process -FilePath $CameraReceiver -ArgumentList @(('"' + $device + '"'), [string]$receiverSeconds) -WindowStyle Hidden -PassThru `
      -RedirectStandardOutput (Join-Path $receiverDir 'receiver.ndjson') -RedirectStandardError (Join-Path $receiverDir 'receiver.stderr.log')
    $null = $receiver.Handle
    if ($receiver.PriorityClass -ne 'Normal') { throw "OS camera receiver is not at Normal priority ($($receiver.PriorityClass))" }
    Write-DrillJson ([ordered]@{ startedUtc = [DateTime]::UtcNow.ToString('o'); deviceId = $device; requestedSeconds = $receiverSeconds; receiverPid = $receiver.Id
        priorityClass = $receiver.PriorityClass.ToString(); receiverSha256 = (Get-FileHash $CameraReceiver).Hash }) 'os-camera\identity.json'
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
        # Plain strings only: see Get-DrillCameraServingProof for the wedge this avoids.
        $serve = [string[]]@(Get-Content 'C:\ProgramData\CoreVideoPro\vcam-serve.log' -Tail 3000 -ErrorAction SilentlyContinue)
        $proof = Get-DrillCameraServingProof -ServeLines $serve -ExpectedDll $expectedDll
        if ($proof) { Write-DrillJson $proof 'camera-serving-identity.json'; break }
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
  $throughout = New-DrillThroughout -Plan $plan
  $progress = @{}
  # #935: measured encoder cost (supplementary). The core is the installed
  # corevideo-native.exe the shell spawned; if it cannot be found, CPU is null.
  $costRecord = New-DrillEncoderCostRecord -LogicalProcessors ([Environment]::ProcessorCount)
  $coreExe = Join-Path $InstallRoot 'corevideo-native.exe'
  $coreProcess = @(Get-Process corevideo-native -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $coreExe }) | Select-Object -First 1
  if (-not $coreProcess) { Write-DrillLog 'WARNING: installed corevideo-native.exe not found; core CPU will be reported as not measured' }
  $lastCpuMs = $null; $lastWallMs = $null; $lastStreams = @()
  $clock = [Diagnostics.Stopwatch]::StartNew(); $start = $null; $end = $null; $startElapsed = $null
  try {
    do {
      $e = Get-DrillEnvelope
      $broken = Test-DrillInvariant -Envelope $e -Plan $plan -ExpectedEpoch $expectedEpoch
      if ($broken) { throw $broken }
      if ($plan.Stream -and $sink.HasExited) { throw 'SRT sink exited during measurement' }
      if ($plan.Camera -and $receiver.HasExited -and -not $end) { throw "OS camera receiver exited during measurement (exit $($receiver.ExitCode))" }
      $elapsedMs = $clock.Elapsed.TotalMilliseconds
      $inInterval = $null -ne $start -and $null -eq $end
      $sample = Update-DrillThroughout -Throughout $throughout -Envelope $e -Plan $plan -Progress $progress -ElapsedMs $elapsedMs -Measuring $inInterval
      $cpuMs = $null
      if ($coreProcess) { try { $coreProcess.Refresh(); if (-not $coreProcess.HasExited) { $cpuMs = $coreProcess.TotalProcessorTime.TotalMilliseconds } } catch { $cpuMs = $null } }
      $streamsNow = @($e.snapshot.recording.streams)
      if ($inInterval) {
        $cost = Get-DrillEncoderCost -PreviousCpuMs $lastCpuMs -CurrentCpuMs $cpuMs -PreviousWallMs $lastWallMs -CurrentWallMs $elapsedMs `
          -LogicalProcessors ([Environment]::ProcessorCount) -PreviousStreams $lastStreams -CurrentStreams $streamsNow
        Add-DrillEncoderCost -Record $costRecord -Interval $cost
      }
      $lastCpuMs = $cpuMs; $lastWallMs = $elapsedMs; $lastStreams = $streamsNow
      $row = Get-DrillSampleRow -Envelope $e -ElapsedMs $elapsedMs -Checks $sample.Checks -Sources $sample.Sources
      $samples.WriteLine(($row | ConvertTo-Json -Depth 12 -Compress)); $samples.Flush()
      if ($null -eq $start -and $clock.Elapsed.TotalSeconds -ge 45) { $start = @{ utc = $row.utc; qpc = $row.qpc }; $startElapsed = $clock.Elapsed.TotalSeconds; Write-DrillLog 'Measured interval started' }
      if ($null -eq $end -and $null -ne $startElapsed -and $clock.Elapsed.TotalSeconds -ge $startElapsed + $Seconds) {
        $end = @{ utc = $row.utc; qpc = $row.qpc }
        $m = [ordered]@{ start = $start; end = $end; qpcFrequency = [Diagnostics.Stopwatch]::Frequency; minimumSeconds = $Seconds
          expectedRemoteIds = $plan.RemoteIds; expectedProgramSourceIds = $plan.ProgramSourceIds }
        foreach ($k in $throughout.Keys) { $m[$k] = $throughout[$k] }
        $m.encoderCost = ConvertTo-DrillEncoderCostJson -Record $costRecord
        Write-DrillJson $m 'measurement.json'
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
  $e.snapshot | ConvertTo-Json -Depth 64 -Compress | Set-Content -LiteralPath (Join-Path $Out 'pre-stop.json') -Encoding UTF8
  $measured = $true
} catch {
  $failure = $_.Exception.Message
  Write-DrillLog "COLLECTION FAILED: $failure"
  Write-DrillJson ([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); error = $failure; at = $_.InvocationInfo.PositionMessage }) 'collection-failure.json'
} finally {
  # Runs on success, on failure and on Ctrl-C. Order: stop outputs and wait for
  # them to finalize, stop the sink, close the app only once the close guard has
  # nothing to ask, and restore preferences only after the app has exited.
  $null = [DrillPowerRequest]::SetThreadExecutionState(2147483648)
  foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process') }
  if ($receiver -and -not $receiver.HasExited) {
    Write-DrillJson @{ utc = [DateTime]::UtcNow.ToString('o'); receiverPid = $receiver.Id; reason = 'Drill stopped early; receiver cancelled, not a completed capture' } 'os-camera\cancelled.json'
    Stop-Process -Id $receiver.Id -ErrorAction SilentlyContinue
  }
  $stopProblems = @()
  if ($app -and -not $app.HasExited) {
    # Collect-Identity order: stream, record (finalize), trace, camera, engine.
    foreach ($action in 'transport.stream.set', 'transport.record.set') { try { Invoke-DrillAction $action @($false) } catch { $stopProblems += "$action failed: $($_.Exception.Message)" } }
    $finalized = Wait-DrillUntil 90 {
      $r = (Get-DrillEnvelope).snapshot.recording
      $null -eq $r -or $null -eq $r.lifecycle -or $r.lifecycle.finalized -or $r.lifecycle.state -in @('completed', 'failed', 'idle')
    }
    if (-not $finalized) { $stopProblems += 'recording did not finalize within 90 s' }
    try {
      $evidence = (Get-DrillEnvelope).snapshot.deliveryEvidence
      if ($evidence.enabled -and -not $evidence.captureComplete -and $evidence.stopEvent) {
        $stopEvent = [Threading.EventWaitHandle]::OpenExisting($evidence.stopEvent); $null = $stopEvent.Set(); $stopEvent.Dispose()
      }
    } catch { $stopProblems += "trace stop failed: $($_.Exception.Message)" }
    # transport.engine.set false on an engine that is already off answers ok (RunTransportSet short-circuits on current == target).
    foreach ($action in 'transport.virtualcam.set', 'transport.engine.set') { try { Invoke-DrillAction $action @($false) } catch { $stopProblems += "$action failed: $($_.Exception.Message)" } }
    if (-not (Wait-DrillUntil 30 { $d = (Get-DrillEnvelope).snapshot.deliveryEvidence; -not $d.enabled -or $d.captureComplete })) { $stopProblems += 'trace did not finalize within 30 s' }
    try { if ((Get-DrillEnvelope).snapshot.meetingState -eq 'in_meeting') { Invoke-DrillAction 'zoom.leave' @() } } catch { }
    $script:closeReasons = @('not evaluated')
    $safe = Wait-DrillUntil 60 { $script:closeReasons = @(Test-DrillSafeToClose (Get-DrillState) (Get-DrillEnvelope).snapshot); $script:closeReasons.Count -eq 0 }
    try { (Get-DrillEnvelope).snapshot | ConvertTo-Json -Depth 64 -Compress | Set-Content -LiteralPath (Join-Path $Out $(if ($measured -and -not $stopProblems.Count) { 'finalized.json' } else { 'stopped.json' })) -Encoding UTF8 } catch { }
    if ($stopProblems.Count -and $measured) {
      $failure = 'Stop/finalize failed: ' + ($stopProblems -join '; ')
      Write-DrillJson ([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); error = $failure }) 'collection-failure.json'
    }
    Write-DrillLog $(if ($stopProblems.Count) { 'Outputs stop problems: ' + ($stopProblems -join '; ') } else { 'Outputs stopped; recording and trace finalized' })
  }
  if ($sink) {
    Start-Sleep -Seconds 5
    if (-not $sink.HasExited) { Stop-Process -Id $sink.Id -ErrorAction SilentlyContinue; $null = $sink.WaitForExit(10000) }
    $bytes = if (Test-Path (Join-Path $Out 'stream-received.ts')) { (Get-Item (Join-Path $Out 'stream-received.ts')).Length } else { 0 }
    Write-DrillJson @{ bytes = $bytes; stoppedUtc = [DateTime]::UtcNow.ToString('o') } 'srt-sink-stopped.json'
  }
  if ($app -and -not $app.HasExited) {
    if ($safe) {
      $null = $app.CloseMainWindow()   # WM_CLOSE: the guard has nothing to ask now
      $null = $app.WaitForExit(45000)
    } else {
      Write-DrillLog ('NOT sending WM_CLOSE: outputs still live (' + ($script:closeReasons -join '; ') + '); it would raise the close prompt')
    }
  }
  if ($app -and -not $app.HasExited) {
    Write-DrillLog 'Waiting up to 10 min for the app to exit before restoring preferences (close it yourself if it asked a question)'
    $null = $app.WaitForExit(600000)
  }
  if ($stagedClip) {
    try { Remove-DrillMediaClip -Path $stagedClip; Write-DrillLog 'Motion clip removed from the media bin' }
    catch { Write-DrillLog "WARNING: staged clip NOT removed from the media bin ($stagedClip): $($_.Exception.Message)" }
  }
  if (-not $app -or $app.HasExited) {
    try {
      Restore-DrillPreferenceFiles -Manifest $manifest
      Write-DrillLog 'Preferences, .bak sidecar and show-input roster restored byte for byte'
      Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
      $restorePending = $true
      Write-DrillLog "ERROR: preferences NOT restored: $($_.Exception.Message)"
    }
  } else {
    $restorePending = $true
  }
  if ($restorePending) {
    Write-DrillJson ([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); appStillRunning = [bool]($app -and -not $app.HasExited)
        backupFolder = $backupDir; files = @($manifest | ForEach-Object { [ordered]@{ path = $_.path; existed = $_.existed; backup = $_.backup; sha256 = $_.sha256 } })
        instruction = 'Close CoreVideo Pro, then copy each backup over its path (delete a path whose existed=false). The app re-saves preferences while running, so do not restore before it exits.' }) 'preferences-restore-pending.json'
    Write-Host ''
    Write-Host '!!! PREFERENCES NOT RESTORED: the app is running on drill preferences (recording into the evidence folder, RTMP off).' -ForegroundColor Red
    Write-Host "!!! See $(Join-Path $Out 'preferences-restore-pending.json'). Backups: $backupDir" -ForegroundColor Red
  }
  try { $self.PriorityClass = $savedPriority } catch { }
}

# ---------------------------------------------------------------- offline verdict
Write-DrillLog 'Judging outputs offline'
& $python (Join-Path $PSScriptRoot 'outputs-drill-judge.py') $Out
$judgeExit = $LASTEXITCODE
if (-not (Test-Path (Join-Path $Out 'verdict.json'))) { throw 'Judge did not write verdict.json' }
Write-DrillLog "Verdict: $((Get-Content (Join-Path $Out 'verdict.json') -Raw | ConvertFrom-Json).overall) (see summary.txt)"
if ($restorePending) { exit 3 }
exit $judgeExit
