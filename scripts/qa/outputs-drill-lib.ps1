# Pure helpers for scripts/qa/outputs-drill.ps1 (#443). Dot-source only.
#
# Everything here is side-effect free except the preference backup/patch/
# restore functions, which touch only the file path they are given (the tests
# run them against a temp file). The live orchestration lives in
# outputs-drill.ps1; the offline judging in outputs-drill-judge.py.

# No Set-StrictMode: snapshots legitimately omit keys (an older peer, a skipped
# output) and strict property access would turn absence into a throw.

# Slots and scene routes copied from the 2026-10-22 qualification harness
# (Prepare-LiveWorkload.ps1): remotes on 1-5,7-9, screen on 6, webcam on 10,
# Program = speaker-slides with the webcam and the screen.
$script:DrillRemoteSlots = @(1, 2, 3, 4, 5, 7, 8, 9)
$script:DrillScreenSlot = 6
$script:DrillWebcamSlot = 10
$script:DrillScreenSourceId = 'capture:screen:0'

function ConvertTo-DrillIdList {
  param([string[]]$Ids = @())
  return , @($Ids | ForEach-Object { "$_" -split '[,\s]+' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Test-DrillParameters {
  param([int]$Seconds, [string]$Out, [string[]]$RemoteIds = @(), [string]$MeetingUrl, [string]$WebcamSourceId, [int]$ProbeMaxSessions = 0)
  $errors = [System.Collections.Generic.List[string]]::new()
  # 0 = not set. Otherwise the COREVIDEO_ENCODER_PROBE_MAX_SESSIONS QA override (discovery depth only).
  if ($ProbeMaxSessions -ne 0 -and ($ProbeMaxSessions -lt 1 -or $ProbeMaxSessions -gt 16)) { $errors.Add("ProbeMaxSessions must be between 1 and 16; got $ProbeMaxSessions.") }
  if ($Seconds -lt 60 -or $Seconds -gt 7200) { $errors.Add('Seconds must be between 60 and 7200 (the OS camera receiver caps at 7500 s including warmup).') }
  if ([string]::IsNullOrWhiteSpace($Out)) { $errors.Add('Out is required.') }
  elseif (-not [IO.Path]::IsPathRooted($Out) -or ([IO.Path]::GetPathRoot($Out)).TrimEnd('\', '/').ToUpperInvariant() -ne 'D:') {
    $errors.Add("Out must be an absolute path on D: (evidence is large); got '$Out'.")
  }
  $ids = @($RemoteIds | Where-Object { $_ -ne $null -and $_ -ne '' })
  if ($ids.Count -gt $script:DrillRemoteSlots.Count) { $errors.Add("At most $($script:DrillRemoteSlots.Count) remote ids are supported (slots 1-5,7-9).") }
  foreach ($id in $ids) { if ($id -notmatch '^[0-9]+$') { $errors.Add("Remote id '$id' must be a numeric Zoom participant id.") } }
  if (@($ids | Select-Object -Unique).Count -ne $ids.Count) { $errors.Add('Remote ids must be unique.') }
  if ($ids.Count -gt 0 -and [string]::IsNullOrWhiteSpace($MeetingUrl)) { $errors.Add('MeetingUrl (the full link with pwd) is required with RemoteIds.') }
  if ($WebcamSourceId -and $WebcamSourceId -notmatch '^capture:[0-9a-f]{16}$') { $errors.Add("WebcamSourceId must look like capture:<16 hex>; got '$WebcamSourceId'.") }
  return , $errors.ToArray()
}

function Resolve-DrillInstallRoot {
  # The Start-menu "CoreVideo Pro" shortcut targets <root>\StartCoreVideo.cmd.
  param([string]$ShortcutPath = (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\CoreVideo Pro.lnk'))
  if (-not (Test-Path -LiteralPath $ShortcutPath)) { throw "Start-menu shortcut not found: $ShortcutPath. Pass -InstallRoot." }
  $link = (New-Object -ComObject WScript.Shell).CreateShortcut($ShortcutPath)
  if ([IO.Path]::GetFileName($link.TargetPath) -ne 'StartCoreVideo.cmd') { throw "Shortcut does not target StartCoreVideo.cmd: $($link.TargetPath)" }
  return (Split-Path -Parent $link.TargetPath)
}

function Get-DrillWebcamFromPreferences {
  # The single non-screen capture id the operator's profile already knows.
  param([Parameter(Mandatory)]$Preferences)
  $ids = [System.Collections.Generic.List[string]]::new()
  foreach ($name in 'IsoRecordingSourceIds') {
    if ($Preferences.PSObject.Properties[$name]) { foreach ($id in @($Preferences.$name)) { $ids.Add([string]$id) } }
  }
  foreach ($name in 'SourceDisplayNames', 'SourceSecondaryLines') {
    $map = if ($Preferences.PSObject.Properties[$name]) { $Preferences.$name } else { $null }
    if ($map -and $map.PSObject) { foreach ($p in $map.PSObject.Properties) { $ids.Add($p.Name) } }
  }
  return @($ids | Where-Object { $_ -match '^capture:[0-9a-f]{16}$' } | Select-Object -Unique)
}

function Test-DrillWebcamPresent {
  # The webcam named in the profile must be in the app's capture-device list,
  # or the shell never tries to connect it and the readiness loop waits its
  # full 300 s for a device that is not there (2026-10-25 shakeout: the
  # profile's webcam was unplugged). Returns the reasons; empty = present.
  param([Parameter(Mandatory)][string]$WebcamSourceId, [object[]]$Devices = @())
  $id = $WebcamSourceId -replace '^capture:', ''
  $match = @($Devices | Where-Object { $_.id -eq $id })
  if (-not $match.Count) {
    $cams = @($Devices | Where-Object { $_.vendor -eq 'uvc' } | ForEach-Object { "capture:$($_.id) ($($_.name))" })
    return @("Webcam $WebcamSourceId is not in the app's capture-device list (unplugged or renamed); UVC devices present: " + $(if ($cams.Count) { $cams -join ', ' } else { 'none' }) + '. Pass -WebcamSourceId or plug the camera in.')
  }
  return @()
}

function Get-DrillWorkloadPlan {
  param([string[]]$RemoteIds = @(), [Parameter(Mandatory)][string]$WebcamSourceId, [bool]$Stream = $true, [bool]$Camera = $true, [string]$RecordingFolder)
  $ids = @($RemoteIds | Where-Object { $_ })
  $assignments = @()
  for ($i = 0; $i -lt $ids.Count; $i++) { $assignments += [pscustomobject]@{ slot = $script:DrillRemoteSlots[$i]; source = 'zoom:' + $ids[$i] } }
  $assignments += [pscustomobject]@{ slot = $script:DrillScreenSlot; source = $script:DrillScreenSourceId }
  $assignments += [pscustomobject]@{ slot = $script:DrillWebcamSlot; source = $WebcamSourceId }
  # ISO identity is stamped only by Zoom and UVC sources (QaIsoIdentity.h call
  # sites); screen capture has no QA ISO identity, so it is not ISO-recorded.
  # [string[]] keeps a one-ISO plan an array: assigning the output of an if
  # statement unrolls a single-element array, drill.json then carries a bare
  # string and the judge iterates its characters (seen 2026-10-25 shakeout).
  [string[]]$iso = if ($ids.Count) { @($ids | ForEach-Object { 'zoom:' + $_ }) } else { @($WebcamSourceId) }
  $local = $ids.Count -eq 0
  [pscustomobject]@{
    RemoteIds        = $ids
    LocalOnly        = $local
    Assignments      = $assignments
    Scene            = 'speaker-slides'
    Routes           = ('{"speaker-slides-1":' + $script:DrillWebcamSlot + ',"speaker-slides-2":' + $script:DrillScreenSlot + '}')
    ProgramSourceIds = @(@($WebcamSourceId, $script:DrillScreenSourceId) | Sort-Object)
    CaptureSourceIds = @($WebcamSourceId, $script:DrillScreenSourceId)
    IsoSourceIds     = $iso
    # The capture engine is the Zoom media engine: transport.engine.set is
    # refused (HTTP 422) outside a meeting, and local sources route to Program
    # without it (#1009). Only a remote-feed plan turns it on.
    EngineRequired   = ($ids.Count -gt 0)
    Stream           = $Stream
    Camera           = $Camera
    RecordingFolder  = $RecordingFolder
    Workload         = if ($local) { 'local sources only: webcam + screen capture in Program, webcam ISO; no remote Zoom feeds; media not included' }
                       else { "$($ids.Count) remote Zoom feed(s) as ISOs + webcam and screen capture in Program; media not included" }
  }
}

function Set-DrillJsonProperty {
  param($Object, [string]$Name, $Value)
  if ($Object.PSObject.Properties[$Name]) { $Object.$Name = $Value }
  else { $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
}

# The app's model (ProductionOutputPreferencesStore.cs) types these fields. A
# JSON number where the model has `string?` (StreamSrtPort, StreamSrtLatencyMs,
# StreamSrtKeyLength) makes System.Text.Json throw; the store then reports
# Corrupt, recovers from the .bak sidecar and re-saves it over the primary, so a
# wrongly typed patch is silently discarded (PR #1024 review). Every value the
# drill writes is therefore typed here, and Get-DrillJsonKindChanges refuses any
# patch that changes the JSON kind of an existing value.
$script:DrillPatchTypes = @{
  RecordingTargetFolder = 'string'; RecordingFilenamePrefix = 'string'; IsoRecordingEnabled = 'bool'
  IsoRecordingSourceIds = 'array'; StreamRtmpEnabled = 'bool'; StreamSrtEnabled = 'bool'
  StreamSrtMode = 'string'; StreamSrtHost = 'string'; StreamSrtPort = 'string'
}

function Get-DrillJsonKind($Value) {
  if ($null -eq $Value) { return 'null' }
  if ($Value -is [string]) { return 'string' }
  if ($Value -is [bool]) { return 'bool' }
  if ($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal]) { return 'number' }
  if ($Value -is [System.Collections.IList]) { return 'array' }
  return 'object'
}

function Get-DrillJsonKindChanges {
  # Recursive: every value present on both sides must keep its JSON kind.
  param($Before, $After, [string]$Path = '$')
  $kb = Get-DrillJsonKind $Before; $ka = Get-DrillJsonKind $After
  if ($kb -ne $ka -and $kb -ne 'null' -and $ka -ne 'null') { return @("$Path : $kb -> $ka") }
  $changes = @()
  if ($kb -eq 'object' -and $ka -eq 'object') {
    foreach ($p in $Before.PSObject.Properties) {
      if ($After.PSObject.Properties[$p.Name]) { $changes += Get-DrillJsonKindChanges $p.Value $After.($p.Name) "$Path.$($p.Name)" }
    }
  } elseif ($kb -eq 'array' -and $ka -eq 'array' -and $Before.Count -eq $After.Count) {
    for ($i = 0; $i -lt $Before.Count; $i++) { $changes += Get-DrillJsonKindChanges $Before[$i] $After[$i] "$Path[$i]" }
  }
  return $changes
}

function Get-DrillPreferenceFiles {
  # The primary plus the app's own AtomicJsonFile sidecar (Path + ".bak"),
  # which holds the previous primary and so holds drill values after a run.
  param([Parameter(Mandatory)][string]$Path)
  return @($Path, ($Path + '.bak'))
}

function Backup-DrillPreferenceFiles {
  # Byte copies OUTSIDE the evidence folder (the files carry the DPAPI RTMP key).
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$BackupDirectory)
  if (-not (Test-Path -LiteralPath $Path)) { throw "Preferences file not found: $Path" }
  if ((Test-Path -LiteralPath $BackupDirectory) -and @(Get-ChildItem -LiteralPath $BackupDirectory -Force).Count) { throw "Refusing to reuse a non-empty backup folder: $BackupDirectory" }
  $null = New-Item -ItemType Directory -Path $BackupDirectory -Force
  $manifest = @()
  foreach ($file in Get-DrillPreferenceFiles $Path) {
    $entry = [ordered]@{ path = $file; existed = [bool](Test-Path -LiteralPath $file); backup = $null; sha256 = $null }
    if ($entry.existed) {
      $entry.backup = Join-Path $BackupDirectory ([IO.Path]::GetFileName($file))
      Copy-Item -LiteralPath $file -Destination $entry.backup
      $entry.sha256 = (Get-FileHash -LiteralPath $entry.backup -Algorithm SHA256).Hash
    }
    $manifest += [pscustomobject]$entry
  }
  $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BackupDirectory 'manifest.json') -Encoding UTF8
  return , $manifest
}

function Restore-DrillPreferenceFiles {
  # Puts every file back exactly; a file that did not exist before is removed.
  param([Parameter(Mandatory)]$Manifest)
  foreach ($entry in $Manifest) {
    if ($entry.existed) {
      Copy-Item -LiteralPath $entry.backup -Destination $entry.path -Force
      $actual = (Get-FileHash -LiteralPath $entry.path -Algorithm SHA256).Hash
      if ($actual -ne $entry.sha256) { throw "Restore mismatch for $($entry.path): $actual vs backup $($entry.sha256)" }
    } elseif (Test-Path -LiteralPath $entry.path) {
      Remove-Item -LiteralPath $entry.path -Force
    }
  }
}

function Set-DrillPreferences {
  # Points recording at the evidence folder and SRT at a loopback caller and
  # forces RTMP off. The stored RTMP key and URL are never changed; the key is
  # read only to prove the rewritten file still carries it unchanged.
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$RecordingFolder,
        [Parameter(Mandatory)][string[]]$IsoSourceIds, [int]$SrtPort, [switch]$Stream)
  $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
  $prefs = $raw | ConvertFrom-Json
  $original = $raw | ConvertFrom-Json
  $values = [ordered]@{
    RecordingTargetFolder = [string]$RecordingFolder; RecordingFilenamePrefix = 'outputs-drill'
    IsoRecordingEnabled = $true; IsoRecordingSourceIds = [object[]]@($IsoSourceIds | ForEach-Object { [string]$_ })
    StreamRtmpEnabled = $false
  }
  if ($Stream) {
    if ($SrtPort -lt 1024 -or $SrtPort -gt 65535) { throw "Invalid SRT port $SrtPort" }
    $values.StreamSrtEnabled = $true; $values.StreamSrtMode = 'caller'; $values.StreamSrtHost = '127.0.0.1'
    $values.StreamSrtPort = [string]$SrtPort   # string? in the model; a JSON number is rejected
  }
  foreach ($name in @($values.Keys)) {
    if ((Get-DrillJsonKind $values[$name]) -ne $script:DrillPatchTypes[$name]) { throw "Internal error: $name would be written as $(Get-DrillJsonKind $values[$name])" }
    Set-DrillJsonProperty $prefs $name $values[$name]
  }
  $json = $prefs | ConvertTo-Json -Depth 100
  $reparsed = $json | ConvertFrom-Json
  $changes = @(Get-DrillJsonKindChanges $original $reparsed)
  if ($changes.Count) { throw ('Refusing a preferences patch that changes JSON types (the app would discard it): ' + ($changes -join '; ')) }
  $keyBefore = if ($original.PSObject.Properties['StreamRtmpStreamKey']) { [string]$original.StreamRtmpStreamKey } else { $null }
  $keyAfter = if ($reparsed.PSObject.Properties['StreamRtmpStreamKey']) { [string]$reparsed.StreamRtmpStreamKey } else { $null }
  if ($keyAfter -cne $keyBefore) { throw 'Internal error: the stored RTMP key would change' }
  $json | Set-Content -LiteralPath $Path -Encoding UTF8
  return [pscustomobject]@{ Preferences = $reparsed; Expected = [pscustomobject]$values }
}

function Test-DrillPreferencesApplied {
  # After the app has loaded its profile the file must still hold the patch,
  # with the same types. Had the app rejected it, it would have recovered from
  # .bak and re-saved the owner's previous values over the primary.
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Expected)
  $problems = @()
  try { $now = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return @("preferences unreadable: $($_.Exception.Message)") }
  foreach ($p in $Expected.PSObject.Properties) {
    if (-not $now.PSObject.Properties[$p.Name]) { $problems += "$($p.Name) missing"; continue }
    $actual = $now.($p.Name)
    if ((Get-DrillJsonKind $actual) -ne (Get-DrillJsonKind $p.Value)) { $problems += "$($p.Name) is a $(Get-DrillJsonKind $actual), expected $(Get-DrillJsonKind $p.Value)"; continue }
    $same = if ((Get-DrillJsonKind $actual) -eq 'array') { (@($actual) -join '|') -ceq (@($p.Value) -join '|') } else { $actual -ceq $p.Value }
    if (-not $same) { $problems += "$($p.Name) is '$actual', expected '$($p.Value)'" }
  }
  return $problems   # callers wrap in @()
}

function Test-DrillSafeToClose {
  # The shell's close guard (CloseGuardPolicy.cs) prompts "Stop outputs and
  # close?" and activates the window while recording or streaming is live or
  # finalizing. WM_CLOSE is sent only when this returns no reasons.
  param($State, $Snapshot)
  $reasons = @()
  if ($null -eq $State -or $null -eq $Snapshot) { return @('state or snapshot unavailable') }
  if ($State.recording) { $reasons += 'recording requested/observed' }
  if ($State.streaming) { $reasons += 'streaming requested/observed' }
  if ($State.engineOn) { $reasons += 'engine on' }
  $rec = if ($Snapshot.recording -and $Snapshot.recording.lifecycle) { $Snapshot.recording.lifecycle.state } else { $null }
  if ($null -ne $rec -and $rec -notin @('completed', 'failed', 'idle')) { $reasons += "recording lifecycle '$rec'" }
  if ($Snapshot.outputSenderSession) {
    if ([int]$Snapshot.outputSenderSession.activeSenderCount -ne 0) { $reasons += "activeSenderCount $($Snapshot.outputSenderSession.activeSenderCount)" }
    foreach ($sender in @($Snapshot.outputSenderSession.senders)) {
      $ls = if ($sender.PSObject.Properties['lifecycle'] -and $sender.lifecycle) { $sender.lifecycle.state } else { $null }
      if ($null -ne $ls -and $ls -notin @('completed', 'failed', 'idle')) { $reasons += "sender lifecycle '$ls'" }
    }
  }
  return $reasons   # callers wrap in @()
}

function Get-DrillFreeUdpPort {
  # SRT runs over UDP. Bind port 0 on loopback, read the OS choice, release it.
  $client = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
  try { return $client.Client.LocalEndPoint.Port } finally { $client.Dispose() }
}

function New-DrillSrtSinkUrl {
  # Listener bound to loopback only; never a public interface.
  param([Parameter(Mandatory)][int]$Port, [int]$LatencyMs = 600, [int]$ListenTimeoutSeconds = 2400, [string]$Passphrase, [int]$KeyLength)
  $url = "srt://127.0.0.1:$Port`?mode=listener&transtype=live&latency=$($LatencyMs * 1000)&listen_timeout=$($ListenTimeoutSeconds * 1000000)"
  if ($Passphrase) {
    $url += '&passphrase=' + [Uri]::EscapeDataString($Passphrase)
    if ($KeyLength -gt 0) { $url += '&pbkeylen=' + $KeyLength }
  }
  return $url
}

function Test-DrillAdmissionsReady {
  param($Snapshot)
  $admissions = @($Snapshot.programSourceAdmission.sources)
  return ($Snapshot.programFrame.health -eq 'live' -and $Snapshot.programSourceAdmission.version -eq 1 -and $admissions.Count -gt 0 -and
    @($admissions | Where-Object { $_.state -ne 'ready' -or $null -eq $_.actualFrameId -or $_.actualEpoch -le 0 -or $_.actualCapture100ns -le 0 -or
      $_.actualEpoch -ne $_.requestedEpoch -or $_.actualFrameId -ne $_.requestedFrameId -or $_.actualCapture100ns -ne $_.requestedCapture100ns }).Count -eq 0)
}

function Test-DrillRemoteLive1080 {
  param($Snapshot, [string]$Id)
  $src = @($Snapshot.zoomSubscriptionChurn.sources | Where-Object { $_.kind -eq 'participant-video' -and $_.participantId -eq $Id })
  return ($src.Count -eq 1 -and $src[0].receivedWidth -eq 1920 -and $src[0].receivedHeight -eq 1080 -and $src[0].coreFramesIngested -gt 0 -and
    $null -ne $src[0].lastCoreFrameAgeMs -and $src[0].lastCoreFrameAgeMs -lt 1000)
}

function Get-DrillStreamProblems {
  # Run-PictureDiagnostic.ps1: one sender, delivering, fps >= 55 when reported,
  # no retries, no warnings; plus: never an RTMP destination.
  param($Snapshot)
  $problems = @()
  $session = $Snapshot.outputSenderSession
  $senders = @($session.senders)
  foreach ($sender in $senders) { if ("$($sender.destination)" -match '(?i)rtmp') { $problems += 'RTMP sender present' } }
  if ($null -eq $session -or $session.activeSenderCount -ne 1 -or $senders.Count -ne 1 -or $senders[0].framesSent -le 0) { $problems += 'stream sender not delivering'; return $problems }
  if ($null -ne $senders[0].fps -and $senders[0].fps -lt 55) { $problems += "stream fps $($senders[0].fps) < 55" }
  if ($senders[0].retryCount -gt 0) { $problems += "stream retryCount $($senders[0].retryCount)" }
  if (@($session.warnings).Count) { $problems += 'stream warnings: ' + (@($session.warnings) -join '; ') }
  return $problems   # callers wrap in @()
}

function Test-DrillSameFolder([string]$A, [string]$B) {
  if (-not $A -or -not $B) { return $false }
  return $A.TrimEnd('\', '/').Equals($B.TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)
}

function Test-DrillReadiness {
  # One readiness sample, as Wait-Ready-Refreshed.ps1 + Run-PictureDiagnostic.ps1
  # judged it, generalized to the plan. Returns Ready, Fatal and Reasons.
  param([Parameter(Mandatory)]$Envelope, [Parameter(Mandatory)]$Plan)
  $reasons = [System.Collections.Generic.List[string]]::new()
  $s = $Envelope.snapshot
  if (-not $Envelope.available -or $Envelope.ageMs -ge 1000) { $reasons.Add('snapshot unavailable or stale') }
  if ($null -eq $s) { return [pscustomobject]@{ Ready = $false; Fatal = $null; Reasons = @('no snapshot') } }
  foreach ($id in $Plan.RemoteIds) { if (-not (Test-DrillRemoteLive1080 $s $id)) { $reasons.Add("remote $id not live at 1920x1080") } }
  foreach ($id in $Plan.CaptureSourceIds) {
    $src = @($s.sources | Where-Object { $_.sourceId -eq $id })
    if ($src.Count -ne 1 -or -not $src[0].hasVideo -or [long]$src[0].framesIngested -le 0) { $reasons.Add("capture $id not ingesting") }
  }
  $fatal = $null
  if ($s.recording.lifecycle.state -eq 'failed') { $fatal = 'Recording failed' }
  if ($s.recording.lifecycle.state -ne 'producing') { $reasons.Add('recording not producing') }
  if ($Plan.RecordingFolder -and $s.recording.targetFolder -and -not (Test-DrillSameFolder $s.recording.targetFolder $Plan.RecordingFolder)) {
    $fatal = "Recording is writing to '$($s.recording.targetFolder)', not the drill folder: the preferences patch was not applied"
  }
  $tracks = @($s.recording.streams | Where-Object { $_.kind -eq 'iso' })
  foreach ($id in $Plan.IsoSourceIds) {
    $t = @($tracks | Where-Object { $_.sourceId -eq $id })
    if ($t.Count -ne 1 -or $t[0].readiness -ne 'ready' -or $t[0].framesWritten -le 0) { $reasons.Add("ISO $id not writing") }
  }
  if (-not (Test-DrillAdmissionsReady $s)) { $reasons.Add('Program source admissions not ready') }
  $actual = (@($s.programSourceAdmission.sources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
  if ($actual -ne ($Plan.ProgramSourceIds -join '|')) { $reasons.Add("Program sources are '$actual'") }
  $stream = @(Get-DrillStreamProblems $s)
  if (@($stream | Where-Object { $_ -eq 'RTMP sender present' }).Count) { $fatal = 'An RTMP sender is active; the drill never streams to a public endpoint' }
  if ($Plan.Stream) {
    foreach ($p in $stream) { $reasons.Add($p) }
    if (@($stream | Where-Object { $_ -like 'stream retryCount*' }).Count) { $fatal = 'Stream to the local SRT sink retried before measurement' }
  }
  return [pscustomobject]@{ Ready = ($reasons.Count -eq 0); Fatal = $fatal; Reasons = $reasons.ToArray() }
}

function Test-DrillInvariant {
  # Collection-time aborts from Collect-Identity.ps1. Returns $null or the break.
  param([Parameter(Mandatory)]$Envelope, [Parameter(Mandatory)]$Plan, $ExpectedEpoch)
  $s = $Envelope.snapshot
  if (-not $Envelope.available -or $Envelope.ageMs -gt 1000 -or $null -eq $s) { return 'Snapshot stale/unavailable' }
  if ($null -ne $ExpectedEpoch -and $s.graphics.coreEpoch -ne $ExpectedEpoch) { return 'Native core restarted' }
  if ($s.recording.lifecycle.state -ne 'producing') { return 'Recording stopped producing' }
  $actual = (@($s.programSourceAdmission.sources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
  if ($actual -ne ($Plan.ProgramSourceIds -join '|')) { return "Program source inventory changed to '$actual'" }
  $tracks = @($s.recording.streams | Where-Object { $_.kind -eq 'iso' -and $_.framesWritten -gt 0 })
  foreach ($id in $Plan.IsoSourceIds) { if (-not @($tracks | Where-Object { $_.sourceId -eq $id }).Count) { return "ISO writer for $id disappeared" } }
  if (@(Get-DrillStreamProblems $s | Where-Object { $_ -eq 'RTMP sender present' }).Count) { return 'An RTMP sender became active' }
  return $null
}

function New-DrillThroughout {
  # Throughout-flags of Collect-Identity.ps1 (lines 75-94), accumulated over the
  # declared interval and written to measurement.json; any false GATING flag
  # fails the drill. #935 (owner ruling D8): an ISO on the software encoder is a
  # note, so hardwareEncodersThroughout (Program AND every ISO on hardware) is a
  # recorded FACT that never fails the run; Program on the software encoder
  # stays a FAIL for RC through programEncoderHardwareThroughout. encoderPlacement
  # lists every encoderPath each ISO reported, and whether it ever changed.
  param([Parameter(Mandatory)]$Plan)
  $flags = [ordered]@{ samples = 0; hardwareEncodersThroughout = $true; programEncoderHardwareThroughout = $true
    remotes1080Throughout = $true; captureProgressThroughout = $true; programSourcesAdmittedThroughout = $true
    encoderPlacement = @{}; firstFailures = @() }
  if ($Plan.Stream) { $flags.streamHealthyThroughout = $true }
  return $flags
}

# Facts recorded beside the throughout flags that never fail the run (#935, D8).
$script:DrillThroughoutFacts = @('hardwareEncodersThroughout')

function Update-DrillThroughout {
  # $Progress (hashtable) carries per-capture last-advance times between calls.
  # $Measuring = $false only refreshes progress (settle period before the interval).
  param([Parameter(Mandatory)]$Throughout, [Parameter(Mandatory)]$Envelope, [Parameter(Mandatory)]$Plan,
        [Parameter(Mandatory)][hashtable]$Progress, [double]$ElapsedMs, [bool]$Measuring = $true)
  $s = $Envelope.snapshot
  $checks = [ordered]@{}
  $iso = @($s.recording.streams | Where-Object { $_.kind -eq 'iso' })
  # #935 B1: MEASURED. recording.encoder.encoderPath is read back from the encoder
  # MFT the Program writer instantiated; hardwareAccelerated/hardwareRequested
  # alone never prove hardware. Absent = not measured = not hardware.
  $programHardware = $null -ne $s.recording -and $null -ne $s.recording.encoder -and
    $s.recording.encoder.PSObject.Properties['encoderPath'] -and $s.recording.encoder.encoderPath -eq 'hardware'
  $checks.hardwareEncodersThroughout = $programHardware -and
    @($Plan.IsoSourceIds | Where-Object { $id = $_; -not @($iso | Where-Object { $_.sourceId -eq $id -and $_.encoderPath -eq 'hardware' -and $_.framesWritten -gt 0 }).Count }).Count -eq 0
  $checks.programEncoderHardwareThroughout = $programHardware
  $checks.remotes1080Throughout = @($Plan.RemoteIds | Where-Object { -not (Test-DrillRemoteLive1080 $s $_) }).Count -eq 0
  $captureOk = $true
  $captures = foreach ($id in $Plan.CaptureSourceIds) {
    $src = @($s.sources | Where-Object { $_.sourceId -eq $id })
    $count = if ($src.Count -eq 1) { [long]$src[0].framesIngested } else { 0 }
    if (-not $Progress.ContainsKey($id) -or $count -gt $Progress[$id].count) { $Progress[$id] = @{ count = $count; at = $ElapsedMs } }
    $age = $ElapsedMs - $Progress[$id].at
    $ready = $src.Count -eq 1 -and $count -gt 0 -and $src[0].width -ge 1920 -and $src[0].height -ge 1080 -and [bool]$src[0].hasVideo -and $age -lt 2000
    if (-not $ready) { $captureOk = $false }
    [ordered]@{ id = $id; frames = $count; progressAgeMs = $age; ready = $ready }
  }
  $checks.captureProgressThroughout = $captureOk
  $checks.programSourcesAdmittedThroughout = Test-DrillAdmissionsReady $s
  if ($Plan.Stream) { $checks.streamHealthyThroughout = @(Get-DrillStreamProblems $s).Count -eq 0 }
  if ($Measuring) {
    $Throughout.samples++
    foreach ($name in $checks.Keys) {
      if (-not $checks[$name] -and $Throughout[$name]) {
        $Throughout[$name] = $false
        if ($script:DrillThroughoutFacts -contains $name) { continue }
        if (@($Throughout.firstFailures).Count -lt 16) { $Throughout.firstFailures += [ordered]@{ flag = $name; utc = [DateTime]::UtcNow.ToString('o'); elapsedMs = $ElapsedMs } }
      }
    }
    if ($null -eq $Throughout.encoderPlacement) { $Throughout.encoderPlacement = @{} }
    foreach ($id in $Plan.IsoSourceIds) {
      $track = @($iso | Where-Object { $_.sourceId -eq $id }) | Select-Object -First 1
      $path = if ($track -and $track.PSObject.Properties['encoderPath'] -and $track.encoderPath) { [string]$track.encoderPath } else { 'not reported' }
      if (-not $Throughout.encoderPlacement.ContainsKey($id)) { $Throughout.encoderPlacement[$id] = [ordered]@{ paths = @(); changed = $false; notReportedSamples = 0 } }
      $entry = $Throughout.encoderPlacement[$id]
      if ($path -eq 'not reported') {
        # Lazy ISO opens report no path until the writer begins; count it, but it is not a path.
        $entry.notReportedSamples = [int]$entry.notReportedSamples + 1
      } elseif (@($entry.paths) -notcontains $path) {
        # Only a MEASURED path following another measured path is a change.
        if (@($entry.paths).Count) { $entry.changed = $true }
        $entry.paths = @($entry.paths) + $path
      }
    }
  }
  return [pscustomobject]@{ Checks = $checks; Captures = @($captures) }
}

function Get-DrillStreamNumber($Stream, [string]$Name) {
  # A counter the snapshot did not report is $null, never 0.
  if ($null -eq $Stream -or -not $Stream.PSObject.Properties[$Name] -or $null -eq $Stream.$Name) { return $null }
  $value = 0.0
  if ([double]::TryParse([string]$Stream.$Name, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$value)) { return $value }
  return $null
}

function Get-DrillEncoderCost {
  # #935: one interval of measured encoder cost, pure. Core CPU % is the core
  # process's CPU time delta over the wall delta, normalized to logical
  # processors. Each SOFTWARE ISO gets its cumulative mean encode work per muxed
  # frame (videoWorkUs / muxVideoFrameCount), the same per-interval mean, and its
  # droppedFrames. Anything not measurable from the two samples is $null, never 0.
  param($PreviousCpuMs, $CurrentCpuMs, $PreviousWallMs, $CurrentWallMs, [int]$LogicalProcessors,
        [object[]]$PreviousStreams = @(), [object[]]$CurrentStreams = @())
  $cpu = $null
  if ($null -ne $PreviousCpuMs -and $null -ne $CurrentCpuMs -and $null -ne $PreviousWallMs -and $null -ne $CurrentWallMs -and
      $LogicalProcessors -gt 0 -and [double]$CurrentWallMs -gt [double]$PreviousWallMs) {
    $cpu = 100.0 * ([double]$CurrentCpuMs - [double]$PreviousCpuMs) / (([double]$CurrentWallMs - [double]$PreviousWallMs) * $LogicalProcessors)
  }
  $software = @{}
  foreach ($stream in @($CurrentStreams | Where-Object { $_ -and $_.kind -eq 'iso' -and $_.encoderPath -eq 'software' })) {
    $work = Get-DrillStreamNumber $stream 'videoWorkUs'
    $frames = Get-DrillStreamNumber $stream 'muxVideoFrameCount'
    $mean = if ($null -ne $work -and $null -ne $frames -and $work -gt 0 -and $frames -gt 0) { $work / $frames } else { $null }
    $interval = $null
    $previous = @($PreviousStreams | Where-Object { $_ -and $_.sourceId -eq $stream.sourceId }) | Select-Object -First 1
    if ($previous) {
      $pw = Get-DrillStreamNumber $previous 'videoWorkUs'; $pf = Get-DrillStreamNumber $previous 'muxVideoFrameCount'
      if ($null -ne $work -and $null -ne $frames -and $null -ne $pw -and $null -ne $pf -and $frames -gt $pf -and $work -ge $pw) {
        $interval = ($work - $pw) / ($frames - $pf)
      }
    }
    $software[[string]$stream.sourceId] = [ordered]@{ isoIndex = $(Get-DrillStreamNumber $stream 'isoIndex'); meanVideoWorkUs = $mean
      intervalVideoWorkUs = $interval; droppedFrames = $(Get-DrillStreamNumber $stream 'droppedFrames'); frameRate = $(Get-DrillStreamNumber $stream 'frameRate') }
  }
  return [pscustomobject]@{ cpuPercent = $cpu; software = $software }
}

function New-DrillEncoderCostRecord {
  param([int]$LogicalProcessors)
  return @{ samples = 0; cpuSamples = 0; cpuSum = 0.0; cpuPeak = $null; logicalProcessors = $LogicalProcessors; software = @{} }
}

function Add-DrillEncoderCost {
  # Accumulates one Get-DrillEncoderCost interval into the run record.
  param([Parameter(Mandatory)]$Record, [Parameter(Mandatory)]$Interval)
  $Record.samples++
  if ($null -ne $Interval.cpuPercent) {
    $Record.cpuSamples++; $Record.cpuSum += [double]$Interval.cpuPercent
    if ($null -eq $Record.cpuPeak -or [double]$Interval.cpuPercent -gt $Record.cpuPeak) { $Record.cpuPeak = [double]$Interval.cpuPercent }
  }
  foreach ($id in @($Interval.software.Keys)) {
    $now = $Interval.software[$id]
    if (-not $Record.software.ContainsKey($id)) {
      $Record.software[$id] = [ordered]@{ isoIndex = $now.isoIndex; meanVideoWorkUs = $null; peakIntervalVideoWorkUs = $null
        droppedFramesStart = $now.droppedFrames; droppedFramesEnd = $now.droppedFrames; frameRate = $now.frameRate }
    }
    $entry = $Record.software[$id]
    if ($null -ne $now.isoIndex) { $entry.isoIndex = $now.isoIndex }
    if ($null -ne $now.frameRate) { $entry.frameRate = $now.frameRate }
    if ($null -ne $now.meanVideoWorkUs) { $entry.meanVideoWorkUs = $now.meanVideoWorkUs }
    if ($null -ne $now.intervalVideoWorkUs -and ($null -eq $entry.peakIntervalVideoWorkUs -or $now.intervalVideoWorkUs -gt $entry.peakIntervalVideoWorkUs)) {
      $entry.peakIntervalVideoWorkUs = $now.intervalVideoWorkUs
    }
    if ($null -eq $entry.droppedFramesStart) { $entry.droppedFramesStart = $now.droppedFrames }
    if ($null -ne $now.droppedFrames) { $entry.droppedFramesEnd = $now.droppedFrames }
  }
}

function ConvertTo-DrillEncoderCostJson {
  # measurement.json `encoderCost`: supplementary, explains a FAIL, never causes one.
  param([Parameter(Mandatory)]$Record)
  $mean = if ($Record.cpuSamples -gt 0) { [math]::Round($Record.cpuSum / $Record.cpuSamples, 1) } else { $null }
  $peak = if ($null -ne $Record.cpuPeak) { [math]::Round($Record.cpuPeak, 1) } else { $null }
  return [ordered]@{ samples = $Record.samples; cpuSamples = $Record.cpuSamples; logicalProcessors = $Record.logicalProcessors
    coreCpuPercent = [ordered]@{ mean = $mean; peak = $peak }; software = $Record.software }
}

function Get-DrillSampleRow {
  # A trimmed per-second row (the reference stored the whole snapshot each
  # second). `snapshot` keeps exactly what program-source-admission-capture.mjs
  # reads (programFrame + programSourceAdmission).
  param($Envelope, [double]$ElapsedMs, $Checks, $Captures)
  $s = $Envelope.snapshot
  [ordered]@{
    utc = [DateTime]::UtcNow.ToString('o'); qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString(); elapsedMs = $ElapsedMs
    ageMs = $Envelope.ageMs; coreEpoch = $s.graphics.coreEpoch; recording = $s.recording.lifecycle.state
    programBuffer = $s.programBuffer; programEncoderHardware = $s.recording.encoder.hardwareAccelerated
    programEncoderPath = $(if ($s.recording.encoder -and $s.recording.encoder.PSObject.Properties['encoderPath']) { $s.recording.encoder.encoderPath } else { $null })
    iso = @($s.recording.streams | ForEach-Object { [ordered]@{ kind = $_.kind; sourceId = $_.sourceId; framesWritten = $_.framesWritten; encoderPath = $_.encoderPath
        isoIndex = $_.isoIndex; videoWorkUs = $_.videoWorkUs; muxVideoFrameCount = $_.muxVideoFrameCount; droppedFrames = $_.droppedFrames } })
    senders = @($s.outputSenderSession.senders | ForEach-Object { [ordered]@{ destination = $_.destination; framesSent = $_.framesSent; fps = $_.fps; retryCount = $_.retryCount; status = $_.status } })
    senderWarnings = @($s.outputSenderSession.warnings)
    virtualCamera = $(if ($s.PSObject.Properties['virtualCamera']) { [ordered]@{ status = $s.virtualCamera.status; framesPublished = $s.virtualCamera.framesPublished } } else { $null })
    captures = $Captures; checks = $Checks
    snapshot = [ordered]@{ programFrame = $s.programFrame; programSourceAdmission = $s.programSourceAdmission }
  }
}
