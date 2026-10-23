# Pester-free tests for scripts/qa/outputs-drill.ps1 (#443), pure parts only.
# Never launches the app, never touches the real preferences file: the patch/
# restore round trip runs on a temp copy with fake secrets. Also runs the
# Python verdict tests (test_outputs_drill.py). Exits non-zero on any failure.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.test.ps1
param([switch]$SkipPython)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')

$script:failures = 0; $script:cases = 0
function Check([string]$Name, [scriptblock]$Body) {
  $script:cases++
  try { & $Body; Write-Host "PASS  $Name" }
  catch { $script:failures++; Write-Host "FAIL  $Name :: $($_.Exception.Message)" -ForegroundColor Red }
}
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

# ---------------------------------------------------------------- parameters
Check 'defaults are valid' { $e = Test-DrillParameters -Seconds 1800 -Out 'D:\CoreVideo-QA\drills\x'; Assert ($e.Count -eq 0) ($e -join ';') }
Check 'seconds below 60 and above 7200 are refused' {
  Assert ((Test-DrillParameters -Seconds 59 -Out 'D:\x').Count -eq 1) '59'
  Assert ((Test-DrillParameters -Seconds 7201 -Out 'D:\x').Count -eq 1) '7201'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x').Count -eq 0) '60'
}
Check 'evidence folder must be absolute on D:' {
  Assert ((Test-DrillParameters -Seconds 60 -Out 'C:\temp\drill').Count -eq 1) 'C: accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'drills\x').Count -eq 1) 'relative accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'd:\lower').Count -eq 0) 'lower-case d: refused'
}
Check 'remote ids: numeric, unique, at most eight, and need a meeting url' {
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -RemoteIds @('1', '2') -MeetingUrl 'https://example.invalid/j/1?pwd=x').Count -eq 0) 'valid refused'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -RemoteIds @('1')).Count -eq 1) 'missing url accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -RemoteIds @('1', 'abc') -MeetingUrl 'u').Count -eq 1) 'non-numeric accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -RemoteIds @('1', '1') -MeetingUrl 'u').Count -eq 1) 'duplicate accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -RemoteIds (1..9 | ForEach-Object { "$_" }) -MeetingUrl 'u').Count -eq 1) 'nine accepted'
}
Check 'webcam id shape' {
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -WebcamSourceId 'capture:f4978a7f6d614c0d').Count -eq 0) 'valid refused'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -WebcamSourceId 'capture:screen:0').Count -eq 1) 'screen accepted as webcam'
}
Check 'id list normalization accepts -File style "1,2 3"' {
  $ids = ConvertTo-DrillIdList @('16778240,16783360', '16784384')
  Assert (($ids -join '|') -eq '16778240|16783360|16784384') ($ids -join '|')
  Assert ((ConvertTo-DrillIdList @()).Count -eq 0) 'empty'
}

# ---------------------------------------------------------------- workload plan
Check 'local-only plan: webcam + screen in Program, webcam ISO only' {
  $p = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d'
  Assert $p.LocalOnly 'not local'
  Assert (($p.IsoSourceIds -join '|') -eq 'capture:f4978a7f6d614c0d') ($p.IsoSourceIds -join '|')
  Assert (($p.ProgramSourceIds -join '|') -eq 'capture:f4978a7f6d614c0d|capture:screen:0') ($p.ProgramSourceIds -join '|')
  Assert ($p.Routes -eq '{"speaker-slides-1":10,"speaker-slides-2":6}') $p.Routes
  Assert ($p.Workload -match 'local sources only') $p.Workload
}
Check 'remote plan: harness slots 1-5,7-9 and zoom ISOs' {
  $p = Get-DrillWorkloadPlan -RemoteIds @('11', '22', '33') -WebcamSourceId 'capture:f4978a7f6d614c0d'
  Assert (-not $p.LocalOnly) 'local'
  Assert ((($p.Assignments | ForEach-Object { "$($_.slot)=$($_.source)" }) -join ',') -eq '1=zoom:11,2=zoom:22,3=zoom:33,6=capture:screen:0,10=capture:f4978a7f6d614c0d') 'slots'
  Assert (($p.IsoSourceIds -join '|') -eq 'zoom:11|zoom:22|zoom:33') 'iso'
}
Check 'webcam discovered from the profile' {
  $prefs = '{"IsoRecordingSourceIds":["zoom:1"],"SourceDisplayNames":{"capture:screen:0":"Display","capture:f4978a7f6d614c0d":"Me","zoom:5":"G"},"SourceSecondaryLines":{"capture:f4978a7f6d614c0d":"x"}}' | ConvertFrom-Json
  $ids = @(Get-DrillWebcamFromPreferences $prefs)
  Assert (($ids -join '|') -eq 'capture:f4978a7f6d614c0d') ($ids -join '|')
}

# ---------------------------------------------------------------- preferences round trip
$temp = Join-Path ([IO.Path]::GetTempPath()) ('outputs-drill-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temp
try {
  $prefsPath = Join-Path $temp 'production-output-preferences.json'
  $backup = Join-Path $temp 'backup.json'
  $original = @'
{
  "Version": 14,
  "StreamRtmpEnabled": true,
  "StreamRtmpServerUrl": "rtmp://a.example.invalid/live2",
  "StreamRtmpStreamKey": "fake-key-\u00e9-do-not-leak",
  "StreamSrtEnabled": false,
  "StreamSrtMode": "listener",
  "StreamSrtHost": "0.0.0.0",
  "StreamSrtPort": 50413,
  "StreamSrtPassphrase": "fake-passphrase",
  "RecordingTargetFolder": "C:\\Users\\someone\\Videos",
  "IsoRecordingEnabled": false,
  "IsoRecordingSourceIds": [],
  "SourceDisplayNames": { "capture:screen:0": "Display \u2014 \u00fc" }
}
'@
  [IO.File]::WriteAllText($prefsPath, $original, [Text.UTF8Encoding]::new($false))
  $originalBytes = [IO.File]::ReadAllBytes($prefsPath)

  Check 'backup records a hash and refuses to overwrite' {
    $script:hash = Backup-DrillPreferences -Path $prefsPath -BackupPath $backup
    Assert ($script:hash -eq (Get-FileHash $prefsPath -Algorithm SHA256).Hash) 'hash'
    $threw = $false; try { $null = Backup-DrillPreferences -Path $prefsPath -BackupPath $backup } catch { $threw = $true }
    Assert $threw 'second backup overwrote the first'
  }
  Check 'patch: recording + ISO + loopback SRT, RTMP forced off, RTMP key untouched' {
    $null = Set-DrillPreferences -Path $prefsPath -RecordingFolder 'D:\CoreVideo-QA\drills\t\recordings' -IsoSourceIds @('capture:f4978a7f6d614c0d') -SrtPort 50999 -Stream
    $raw = [IO.File]::ReadAllText($prefsPath)
    $p = $raw | ConvertFrom-Json
    Assert ($p.RecordingTargetFolder -eq 'D:\CoreVideo-QA\drills\t\recordings') 'folder'
    Assert ($p.StreamRtmpEnabled -eq $false) 'RTMP still enabled'
    Assert ($p.StreamRtmpStreamKey -eq "fake-key-$([char]0xe9)-do-not-leak") 'RTMP key changed'
    Assert ($p.StreamRtmpServerUrl -eq 'rtmp://a.example.invalid/live2') 'RTMP url changed'
    Assert ($p.StreamSrtEnabled -and $p.StreamSrtMode -eq 'caller' -and $p.StreamSrtHost -eq '127.0.0.1' -and $p.StreamSrtPort -eq 50999) 'SRT'
    Assert ($p.StreamSrtPassphrase -eq 'fake-passphrase') 'passphrase changed'
    Assert ($p.IsoRecordingEnabled -and @($p.IsoRecordingSourceIds).Count -eq 1 -and $raw -match '"IsoRecordingSourceIds":\s*\[') 'ISO list must stay a JSON array'
    Assert ($p.SourceDisplayNames.'capture:screen:0' -eq "Display $([char]0x2014) $([char]0xfc)") 'non-ASCII text mangled (encoding)'
  }
  Check 'patch with -Stream off leaves SRT alone but still forces RTMP off' {
    [IO.File]::WriteAllBytes($prefsPath, $originalBytes)
    $null = Set-DrillPreferences -Path $prefsPath -RecordingFolder 'D:\r' -IsoSourceIds @('zoom:1', 'zoom:2')
    $p = Get-Content $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($p.StreamRtmpEnabled -eq $false -and $p.StreamSrtMode -eq 'listener' -and $p.StreamSrtPort -eq 50413) 'SRT touched'
    Assert (@($p.IsoRecordingSourceIds).Count -eq 2) 'iso'
  }
  Check 'patch adds missing keys on an older profile' {
    $old = Join-Path $temp 'old.json'
    [IO.File]::WriteAllText($old, '{"Version": 7}', [Text.UTF8Encoding]::new($false))
    $null = Set-DrillPreferences -Path $old -RecordingFolder 'D:\r' -IsoSourceIds @('zoom:1') -SrtPort 40000 -Stream
    $p = Get-Content $old -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($p.StreamSrtPort -eq 40000 -and $p.StreamRtmpEnabled -eq $false -and $p.RecordingTargetFolder -eq 'D:\r') 'keys not added'
  }
  Check 'restore is byte-for-byte' {
    $null = Restore-DrillPreferences -Path $prefsPath -BackupPath $backup -ExpectedHash $script:hash
    $restored = [IO.File]::ReadAllBytes($prefsPath)
    Assert ($restored.Length -eq $originalBytes.Length -and -not (Compare-Object $restored $originalBytes -SyncWindow 0)) 'bytes differ'
  }
  Check 'restore detects a corrupted backup' {
    [IO.File]::AppendAllText($backup, ' ')
    $threw = $false; try { $null = Restore-DrillPreferences -Path $prefsPath -BackupPath $backup -ExpectedHash $script:hash } catch { $threw = $true }
    Assert $threw 'mismatch not detected'
  }
} finally { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }

# ---------------------------------------------------------------- sink + port
Check 'sink URL is a loopback listener with escaped passphrase' {
  $u = New-DrillSrtSinkUrl -Port 50999 -LatencyMs 600 -ListenTimeoutSeconds 10 -Passphrase 'a b&c' -KeyLength 16
  Assert ($u.StartsWith('srt://127.0.0.1:50999?mode=listener&')) $u
  Assert ($u -match 'latency=600000' -and $u -match 'listen_timeout=10000000' -and $u -match 'passphrase=a%20b%26c' -and $u -match 'pbkeylen=16') $u
  Assert (-not ((New-DrillSrtSinkUrl -Port 5000) -match 'passphrase')) 'passphrase without one'
}
Check 'free UDP port is in the dynamic range and bindable' {
  $port = Get-DrillFreeUdpPort
  Assert ($port -ge 1024 -and $port -le 65535) "$port"
  $c = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, $port)); $c.Dispose()
}

# ---------------------------------------------------------------- readiness + invariants
function New-Envelope([switch]$Stale, [string]$Recording = 'producing', [int]$IsoFrames = 10, [int]$Sent = 5, [int]$Retry = 0, [string]$Webcam = 'capture:f4978a7f6d614c0d') {
  $adm = { param($id) [pscustomobject]@{ sourceId = $id; state = 'ready'; actualFrameId = 9; requestedFrameId = 9; actualEpoch = 1; requestedEpoch = 1; actualCapture100ns = 5; requestedCapture100ns = 5 } }
  [pscustomobject]@{ available = $true; ageMs = $(if ($Stale) { 2000 } else { 40 }); snapshot = [pscustomobject]@{
      graphics = [pscustomobject]@{ coreEpoch = 3 }
      recording = [pscustomobject]@{ lifecycle = [pscustomobject]@{ state = $Recording }; streams = @(
          [pscustomobject]@{ kind = 'program'; framesWritten = 10 },
          [pscustomobject]@{ kind = 'iso'; sourceId = 'capture:f4978a7f6d614c0d'; readiness = 'ready'; framesWritten = $IsoFrames }) }
      sources = @([pscustomobject]@{ sourceId = $Webcam; hasVideo = $true; framesIngested = 100 }, [pscustomobject]@{ sourceId = 'capture:screen:0'; hasVideo = $true; framesIngested = 100 })
      programFrame = [pscustomobject]@{ health = 'live' }
      programSourceAdmission = [pscustomobject]@{ version = 1; sources = @((& $adm 'capture:screen:0'), (& $adm 'capture:f4978a7f6d614c0d')) }
      outputSenderSession = [pscustomobject]@{ activeSenderCount = 1; senders = @([pscustomobject]@{ framesSent = $Sent; retryCount = $Retry }) }
      zoomSubscriptionChurn = [pscustomobject]@{ sources = @() } } }
}
$plan = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d'
Check 'readiness: a healthy local workload is ready' { $r = Test-DrillReadiness -Envelope (New-Envelope) -Plan $plan; Assert $r.Ready ($r.Reasons -join ';') }
Check 'readiness: stale snapshot, idle ISO, silent stream, missing webcam are not ready' {
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Stale) -Plan $plan).Ready) 'stale'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -IsoFrames 0) -Plan $plan).Ready) 'iso'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Sent 0) -Plan $plan).Ready) 'stream'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Webcam 'capture:0000000000000000') -Plan $plan).Ready) 'webcam'
}
Check 'readiness: stream ignored when skipped; failed recording and stream retry are fatal' {
  $noStream = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d' -Stream $false
  Assert (Test-DrillReadiness -Envelope (New-Envelope -Sent 0) -Plan $noStream).Ready 'skipped stream still required'
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Recording failed) -Plan $plan).Fatal -eq 'Recording failed') 'failed'
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Retry 1) -Plan $plan).Fatal) 'retry'
}
Check 'readiness: a remote that is not live at 1080p blocks' {
  $remote = Get-DrillWorkloadPlan -RemoteIds @('77') -WebcamSourceId 'capture:f4978a7f6d614c0d'
  $e = New-Envelope
  $e.snapshot.zoomSubscriptionChurn.sources = @([pscustomobject]@{ kind = 'participant-video'; participantId = '77'; receivedWidth = 1280; receivedHeight = 720; coreFramesIngested = 9; lastCoreFrameAgeMs = 10 })
  Assert (-not (Test-DrillReadiness -Envelope $e -Plan $remote).Ready) '720p accepted'
}
Check 'invariants: healthy is null; core restart, stopped recording, changed Program break' {
  Assert ($null -eq (Test-DrillInvariant -Envelope (New-Envelope) -Plan $plan -ExpectedEpoch 3)) 'healthy'
  Assert ((Test-DrillInvariant -Envelope (New-Envelope) -Plan $plan -ExpectedEpoch 4) -eq 'Native core restarted') 'epoch'
  Assert ((Test-DrillInvariant -Envelope (New-Envelope -Recording finalizing) -Plan $plan -ExpectedEpoch 3) -eq 'Recording stopped producing') 'recording'
  $e = New-Envelope; $e.snapshot.programSourceAdmission.sources = @($e.snapshot.programSourceAdmission.sources[0])
  Assert ((Test-DrillInvariant -Envelope $e -Plan $plan -ExpectedEpoch 3) -match 'Program source inventory changed') 'program'
}
Check 'sample row is compact and carries the QPC' {
  $row = Get-DrillSampleRow -Envelope (New-Envelope) -ElapsedMs 1.5
  Assert ($row.qpc -match '^[0-9]+$' -and $row.coreEpoch -eq 3 -and @($row.iso).Count -eq 2 -and $row.programSourcesReady) 'row'
}

# ---------------------------------------------------------------- verdict aggregation (Python)
if (-not $SkipPython) {
  Check 'verdict aggregation + offline judge orchestration (test_outputs_drill.py)' {
    $python = (Get-Command python -ErrorAction Stop).Source
    $ErrorActionPreference = 'Continue'   # unittest reports on stderr; 5.1 would wrap it as an error
    $output = & $python -W ignore (Join-Path $PSScriptRoot 'test_outputs_drill.py') 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw $output }
    Write-Host ($output.Trim() -split "`n" | Select-Object -Last 3 | Out-String).Trim()
  }
}

Write-Host "`n$($script:cases - $script:failures)/$($script:cases) passed"
if ($script:failures) { exit 1 }
exit 0
