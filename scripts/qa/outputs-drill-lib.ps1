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
  param([int]$Seconds, [string]$Out, [string[]]$RemoteIds = @(), [string]$MeetingUrl, [string]$WebcamSourceId)
  $errors = [System.Collections.Generic.List[string]]::new()
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

function Get-DrillWorkloadPlan {
  param([string[]]$RemoteIds = @(), [Parameter(Mandatory)][string]$WebcamSourceId, [bool]$Stream = $true, [bool]$Camera = $true)
  $ids = @($RemoteIds | Where-Object { $_ })
  $assignments = @()
  for ($i = 0; $i -lt $ids.Count; $i++) { $assignments += [pscustomobject]@{ slot = $script:DrillRemoteSlots[$i]; source = 'zoom:' + $ids[$i] } }
  $assignments += [pscustomobject]@{ slot = $script:DrillScreenSlot; source = $script:DrillScreenSourceId }
  $assignments += [pscustomobject]@{ slot = $script:DrillWebcamSlot; source = $WebcamSourceId }
  # ISO identity is stamped only by Zoom and UVC sources (QaIsoIdentity.h call
  # sites); screen capture has no QA ISO identity, so it is not ISO-recorded.
  $iso = if ($ids.Count) { @($ids | ForEach-Object { 'zoom:' + $_ }) } else { @($WebcamSourceId) }
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
    Stream           = $Stream
    Camera           = $Camera
    Workload         = if ($local) { 'local sources only: webcam + screen capture in Program, webcam ISO; no remote Zoom feeds; media not included' }
                       else { "$($ids.Count) remote Zoom feed(s) as ISOs + webcam and screen capture in Program; media not included" }
  }
}

function Set-DrillJsonProperty {
  param($Object, [string]$Name, $Value)
  if ($Object.PSObject.Properties[$Name]) { $Object.$Name = $Value }
  else { $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
}

function Backup-DrillPreferences {
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$BackupPath)
  if (-not (Test-Path -LiteralPath $Path)) { throw "Preferences file not found: $Path" }
  if (Test-Path -LiteralPath $BackupPath) { throw "Refusing to overwrite an existing backup: $BackupPath" }
  Copy-Item -LiteralPath $Path -Destination $BackupPath
  return (Get-FileHash -LiteralPath $BackupPath -Algorithm SHA256).Hash
}

function Set-DrillPreferences {
  # Points recording at the evidence folder and SRT at a loopback caller.
  # RTMP is forced OFF and its key/URL are never read or written by value.
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$RecordingFolder,
        [Parameter(Mandatory)][string[]]$IsoSourceIds, [int]$SrtPort, [switch]$Stream)
  $prefs = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
  $rtmpKeyBefore = if ($prefs.PSObject.Properties['StreamRtmpStreamKey']) { [string]$prefs.StreamRtmpStreamKey } else { $null }
  Set-DrillJsonProperty $prefs 'RecordingTargetFolder' $RecordingFolder
  Set-DrillJsonProperty $prefs 'RecordingFilenamePrefix' 'outputs-drill'
  Set-DrillJsonProperty $prefs 'IsoRecordingEnabled' $true
  Set-DrillJsonProperty $prefs 'IsoRecordingSourceIds' ([object[]]$IsoSourceIds)
  Set-DrillJsonProperty $prefs 'StreamRtmpEnabled' $false
  if ($Stream) {
    if ($SrtPort -lt 1024 -or $SrtPort -gt 65535) { throw "Invalid SRT port $SrtPort" }
    Set-DrillJsonProperty $prefs 'StreamSrtEnabled' $true
    Set-DrillJsonProperty $prefs 'StreamSrtMode' 'caller'
    Set-DrillJsonProperty $prefs 'StreamSrtHost' '127.0.0.1'
    Set-DrillJsonProperty $prefs 'StreamSrtPort' $SrtPort
  }
  $rtmpKeyAfter = if ($prefs.PSObject.Properties['StreamRtmpStreamKey']) { [string]$prefs.StreamRtmpStreamKey } else { $null }
  if ($rtmpKeyAfter -ne $rtmpKeyBefore) { throw 'Internal error: the stored RTMP key would change' }
  $prefs | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $Path -Encoding UTF8
  return $prefs
}

function Restore-DrillPreferences {
  param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$BackupPath, [Parameter(Mandatory)][string]$ExpectedHash)
  Copy-Item -LiteralPath $BackupPath -Destination $Path -Force
  $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
  if ($actual -ne $ExpectedHash) { throw "Preferences restore mismatch: $actual vs backup $ExpectedHash" }
  return $actual
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

function Test-DrillReadiness {
  # One readiness sample, as Wait-Ready-Refreshed.ps1 + Run-PictureDiagnostic.ps1
  # judged it, generalized to the plan. Returns Ready, Fatal and Reasons.
  param([Parameter(Mandatory)]$Envelope, [Parameter(Mandatory)]$Plan)
  $reasons = [System.Collections.Generic.List[string]]::new()
  $s = $Envelope.snapshot
  if (-not $Envelope.available -or $Envelope.ageMs -ge 1000) { $reasons.Add('snapshot unavailable or stale') }
  if ($null -eq $s) { return [pscustomobject]@{ Ready = $false; Fatal = $null; Reasons = @('no snapshot') } }
  foreach ($id in $Plan.RemoteIds) {
    $src = @($s.zoomSubscriptionChurn.sources | Where-Object { $_.kind -eq 'participant-video' -and $_.participantId -eq $id })
    $ok = $src.Count -eq 1 -and $src[0].receivedWidth -eq 1920 -and $src[0].receivedHeight -eq 1080 -and $src[0].coreFramesIngested -gt 0 -and
      $null -ne $src[0].lastCoreFrameAgeMs -and $src[0].lastCoreFrameAgeMs -lt 1000
    if (-not $ok) { $reasons.Add("remote $id not live at 1920x1080") }
  }
  foreach ($id in $Plan.CaptureSourceIds) {
    $src = @($s.sources | Where-Object { $_.sourceId -eq $id })
    if ($src.Count -ne 1 -or -not $src[0].hasVideo -or [long]$src[0].framesIngested -le 0) { $reasons.Add("capture $id not ingesting") }
  }
  $fatal = $null
  if ($s.recording.lifecycle.state -eq 'failed') { $fatal = 'Recording failed' }
  if ($s.recording.lifecycle.state -ne 'producing') { $reasons.Add('recording not producing') }
  $tracks = @($s.recording.streams | Where-Object { $_.kind -eq 'iso' })
  foreach ($id in $Plan.IsoSourceIds) {
    $t = @($tracks | Where-Object { $_.sourceId -eq $id })
    if ($t.Count -ne 1 -or $t[0].readiness -ne 'ready' -or $t[0].framesWritten -le 0) { $reasons.Add("ISO $id not writing") }
  }
  if (-not (Test-DrillAdmissionsReady $s)) { $reasons.Add('Program source admissions not ready') }
  $actual = (@($s.programSourceAdmission.sources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
  if ($actual -ne ($Plan.ProgramSourceIds -join '|')) { $reasons.Add("Program sources are '$actual'") }
  if ($Plan.Stream) {
    $senders = @($s.outputSenderSession.senders)
    if ($s.outputSenderSession.activeSenderCount -ne 1 -or $senders.Count -ne 1 -or $senders[0].framesSent -le 0) { $reasons.Add('stream sender not delivering') }
    elseif ($senders[0].retryCount -gt 0) { $fatal = 'Stream to the local SRT sink retried before measurement' }
  }
  return [pscustomobject]@{ Ready = ($reasons.Count -eq 0); Fatal = $fatal; Reasons = $reasons.ToArray() }
}

function Test-DrillInvariant {
  # Collection-time invariants from Collect-Identity.ps1. Returns $null or the break.
  param([Parameter(Mandatory)]$Envelope, [Parameter(Mandatory)]$Plan, $ExpectedEpoch)
  $s = $Envelope.snapshot
  if (-not $Envelope.available -or $Envelope.ageMs -gt 1000 -or $null -eq $s) { return 'Snapshot stale/unavailable' }
  if ($null -ne $ExpectedEpoch -and $s.graphics.coreEpoch -ne $ExpectedEpoch) { return 'Native core restarted' }
  if ($s.recording.lifecycle.state -ne 'producing') { return 'Recording stopped producing' }
  $actual = (@($s.programSourceAdmission.sources | ForEach-Object { $_.sourceId }) | Sort-Object) -join '|'
  if ($actual -ne ($Plan.ProgramSourceIds -join '|')) { return "Program source inventory changed to '$actual'" }
  $tracks = @($s.recording.streams | Where-Object { $_.kind -eq 'iso' -and $_.framesWritten -gt 0 })
  foreach ($id in $Plan.IsoSourceIds) { if (-not @($tracks | Where-Object { $_.sourceId -eq $id }).Count) { return "ISO writer for $id disappeared" } }
  return $null
}

function Get-DrillSampleRow {
  # A trimmed per-second row (the reference stored the whole snapshot each second).
  param($Envelope, [double]$ElapsedMs)
  $s = $Envelope.snapshot
  [ordered]@{
    utc = [DateTime]::UtcNow.ToString('o'); qpc = [Diagnostics.Stopwatch]::GetTimestamp().ToString(); elapsedMs = $ElapsedMs
    ageMs = $Envelope.ageMs; coreEpoch = $s.graphics.coreEpoch; recording = $s.recording.lifecycle.state
    programHealth = $s.programFrame.health; programBuffer = $s.programBuffer
    iso = @($s.recording.streams | ForEach-Object { [ordered]@{ kind = $_.kind; sourceId = $_.sourceId; framesWritten = $_.framesWritten; encoderPath = $_.encoderPath } })
    senders = @($s.outputSenderSession.senders | ForEach-Object { [ordered]@{ framesSent = $_.framesSent; retryCount = $_.retryCount; status = $_.status } })
    virtualCamera = $(if ($s.PSObject.Properties['virtualCamera']) { [ordered]@{ status = $s.virtualCamera.status; framesPublished = $s.virtualCamera.framesPublished } } else { $null })
    programSourcesReady = (Test-DrillAdmissionsReady $s)
  }
}
