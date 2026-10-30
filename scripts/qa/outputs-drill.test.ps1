# Pester-free tests for scripts/qa/outputs-drill.ps1 (#443), pure parts only.
# Never launches the app and never touches the real preferences files: the
# patch/restore round trip runs on temp copies with fake secrets. Also runs:
#  - the app's OWN serializer (outputs-drill-prefs-probe, loaded from the
#    installed app by reflection) over the patched fixture, when dotnet and an
#    installed app are present (skipped with a note otherwise);
#  - the Python verdict tests (test_outputs_drill.py).
# Exits non-zero on any failure.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill.test.ps1
param([switch]$SkipPython, [switch]$SkipAppSerializer, [string]$InstallRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')

$script:failures = 0; $script:cases = 0; $script:skipped = @()
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

# ---------------------------------------------------------------- preferences
# A v14-shaped profile: every top-level key of the app's current file, with the
# app model's JSON kinds (StreamSrtPort/LatencyMs/KeyLength are strings) and
# fake values. StreamRtmpStreamKey mimics the DPAPI at-rest form.
$v14 = @'
{
  "ProgramBufferFrames": 2, "Version": 14, "OhgShowEnabled": false, "FfmpegBinDirectory": "C:\\ffmpeg\\bin",
  "StreamRtmpEnabled": true, "StreamNdiEnabled": false, "StreamSrtEnabled": false, "StreamHlsEnabled": false,
  "StreamHlsPlaylistUrl": "", "StreamRtmpProtocol": "rtmp", "StreamRtmpServerUrl": "rtmp://a.example.invalid/live2",
  "StreamRtmpStreamKey": "dpapi:ZmFrZS1rZXktXHUwMGU5LWRvLW5vdC1sZWFr", "StreamNdiProgramName": "CoreVideo Pro Program",
  "StreamNdiGroupName": "public", "StreamSrtMode": "listener", "StreamSrtHost": "0.0.0.0", "StreamSrtPort": "50413",
  "StreamSrtLatencyMs": "600", "StreamSrtStreamId": "", "StreamSrtKeyLength": "0", "StreamSrtPassphrase": "",
  "CanvasResolution": "1920x1080", "CanvasFps": "60", "LocalAudioSourceEnabled": false,
  "SelectedLocalAudioCaptureDeviceId": "3580c06a493f401f", "AudioMonitoringEnabled": false,
  "SelectedAudioMonitorDeviceId": "default-render-output", "AudioMonitorVolume": 0.75,
  "StreamRenderResolution": "1920x1080", "StreamRenderFps": "60", "StreamVideoCodec": "h265",
  "StreamTargetBitrateMbps": 10, "StreamAudioBitrateKbps": 128, "StreamEncoderMode": "nvenc",
  "StreamPlatformProfileId": "custom", "StreamKeyframeIntervalSeconds": 2, "StreamRateControl": "cbr",
  "StreamH264Profile": "high", "StreamBFrames": 2, "StreamAllowEnhancedRtmp": true,
  "RecordingRenderResolution": "1920x1080", "RecordingRenderFps": "60", "RecordingVideoCodec": "h264",
  "RecordingTargetBitrateMbps": 8.2, "RecordingAudioBitrateKbps": 192, "RecordingTargetFolder": "C:\\Users\\someone\\Videos",
  "RecordingWriteQueueDepth": 6, "RecordingFilenamePrefix": "show", "RecordingFormat": "mp4", "RecordingQuality": "high",
  "LowerThirdPosition": "lower-left", "LowerThirdBuildInMs": 700, "LowerThirdBuildOutMs": 550,
  "LowerThirdAppearance": null, "LowerThirdPresets": {}, "BrandDefaultOverlayBehavior": "all-off",
  "VirtualCameraEnabled": false, "VirtualCameraMirror": false, "VirtualCameraFramerEnabled": false, "VirtualCameraName": null,
  "MultiviewLayoutMode": "pgmPvwTop", "MultiviewTileCount": 10, "MultiviewShowLabels": true, "MultiviewShowTally": true,
  "MultiviewShowMeters": false, "MultiviewShowClock": true,
  "SceneBackgroundAssetIds": { "custom-74d5e390": "media-5f953bd23617" },
  "SourceDisplayNames": { "capture:screen:0": "Display \u2014 \u00fc", "capture:f4978a7f6d614c0d": "Presenter" },
  "SourceSecondaryLines": { "capture:f4978a7f6d614c0d": "Host" }, "SourceDropoutPolicies": {}, "VstInsertStates": {},
  "IsoRecordingEnabled": false, "IsoRecordingSourceIds": ["zoom:16778240"], "ZoomAudioMode": "perGuestIso",
  "ZoomCameraMaxResolution": "1080p", "ZoomCameraMaxFps": 60,
  "CustomScenes": [ { "Id": "custom-1", "Name": "One", "Slots": [ { "X": 0.5, "Y": 0, "Routes": [ 1 ] } ] } ],
  "SourceGrades": { "zoom-person:02464A58": null }, "ScopeRegions": { "capture:f4978a7f6d614c0d": null },
  "MasteringCurrent": { "Enabled": false, "TargetIndex": 0, "GlueAmount": 0.5000000074505806, "CeilingDbfs": -1.3, "GlueRatio": 2 },
  "MasteringCompareA": { "Enabled": false, "TargetIndex": 0 }, "MasteringCompareB": { "Enabled": true, "TargetIndex": 0 },
  "MasteringCompareSlot": "A", "MasteringUserPresets": []
}
'@
$temp = Join-Path ([IO.Path]::GetTempPath()) ('outputs-drill-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temp
try {
  $prefsDir = Join-Path $temp 'CoreVideoPro'; $null = New-Item -ItemType Directory -Path $prefsDir
  $prefsPath = Join-Path $prefsDir 'production-output-preferences.json'
  $sidecar = $prefsPath + '.bak'
  $backupDir = Join-Path $temp 'outside-evidence\backup'
  [IO.File]::WriteAllText($prefsPath, $v14, [Text.UTF8Encoding]::new($false))
  [IO.File]::WriteAllText($sidecar, $v14.Replace('"show"', '"previous"'), [Text.UTF8Encoding]::new($false))
  $originalBytes = [IO.File]::ReadAllBytes($prefsPath); $sidecarBytes = [IO.File]::ReadAllBytes($sidecar)

  Check 'backup copies the primary AND the app''s .bak sidecar, refuses a non-empty folder' {
    $script:manifest = Backup-DrillPreferenceFiles -Path $prefsPath -BackupDirectory $backupDir
    Assert (@($script:manifest).Count -eq 2 -and $script:manifest[1].path -eq $sidecar -and $script:manifest[1].existed) 'sidecar not in the manifest'
    Assert (Test-Path (Join-Path $backupDir 'production-output-preferences.json.bak')) 'sidecar not copied'
    $threw = $false; try { $null = Backup-DrillPreferenceFiles -Path $prefsPath -BackupDirectory $backupDir } catch { $threw = $true }
    Assert $threw 'second backup reused the folder'
  }
  Check 'patch writes StreamSrtPort as a JSON string ("50999"), as the app model requires' {
    $script:patch = Set-DrillPreferences -Path $prefsPath -RecordingFolder 'D:\CoreVideo-QA\drills\t\recordings' -IsoSourceIds @('capture:f4978a7f6d614c0d') -SrtPort 50999 -Stream
    $raw = [IO.File]::ReadAllText($prefsPath)
    Assert ($raw -match '"StreamSrtPort":\s*"50999"') 'StreamSrtPort is not the string "50999"'
    Assert ($raw -notmatch '"StreamSrtPort":\s*50999') 'StreamSrtPort written as a number'
    Assert ($raw -match '"StreamSrtLatencyMs":\s*"600"' -and $raw -match '"StreamSrtKeyLength":\s*"0"') 'other SRT strings changed kind'
    Assert ($raw -match '"IsoRecordingSourceIds":\s*\[') 'ISO list must stay a JSON array'
  }
  Check 'patch: recording + ISO + loopback SRT, RTMP forced off, RTMP key/url and passphrase untouched' {
    $p = Get-Content $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($p.RecordingTargetFolder -eq 'D:\CoreVideo-QA\drills\t\recordings') 'folder'
    Assert ($p.StreamRtmpEnabled -eq $false) 'RTMP still enabled'
    Assert ($p.StreamRtmpStreamKey -ceq 'dpapi:ZmFrZS1rZXktXHUwMGU5LWRvLW5vdC1sZWFr') 'RTMP key changed'
    Assert ($p.StreamRtmpServerUrl -eq 'rtmp://a.example.invalid/live2') 'RTMP url changed'
    Assert ($p.StreamSrtEnabled -and $p.StreamSrtMode -eq 'caller' -and $p.StreamSrtHost -eq '127.0.0.1' -and $p.StreamSrtPort -ceq '50999') 'SRT'
    Assert ($p.StreamSrtPassphrase -eq '') 'passphrase changed'
    Assert ($p.SourceDisplayNames.'capture:screen:0' -eq "Display $([char]0x2014) $([char]0xfc)") 'non-ASCII text mangled (encoding)'
  }
  Check 'every value in the patched v14 profile keeps its JSON kind' {
    $changes = @(Get-DrillJsonKindChanges ($v14 | ConvertFrom-Json) (Get-Content $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json))
    Assert ($changes.Count -eq 0) ($changes -join '; ')
  }
  Check 'the kind check catches the original bug (port as a number)' {
    $bad = $v14 | ConvertFrom-Json; $bad.StreamSrtPort = 50999
    $changes = @(Get-DrillJsonKindChanges ($v14 | ConvertFrom-Json) $bad)
    Assert ($changes.Count -eq 1 -and $changes[0] -match 'StreamSrtPort : string -> number') ($changes -join '; ')
  }
  Check 'applied-check passes on the patched file' {
    $problems = @(Test-DrillPreferencesApplied -Path $prefsPath -Expected $script:patch.Expected)
    Assert ($problems.Count -eq 0) ($problems -join '; ')
  }
  Check 'applied-check refuses when the app recovered from .bak (owner values back in place)' {
    $saved = [IO.File]::ReadAllBytes($prefsPath)
    [IO.File]::WriteAllBytes($prefsPath, $sidecarBytes)   # what the store's recovery re-save leaves behind
    $problems = @(Test-DrillPreferencesApplied -Path $prefsPath -Expected $script:patch.Expected)
    Assert (@($problems | Where-Object { $_ -match '^RecordingTargetFolder' }).Count -and @($problems | Where-Object { $_ -match '^StreamRtmpEnabled' }).Count -and @($problems | Where-Object { $_ -match '^StreamSrtPort' }).Count) ($problems -join '; ')
    # Same values but the port as a number: the kind mismatch alone is refused.
    [IO.File]::WriteAllBytes($prefsPath, $saved)
    $j = Get-Content $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json; $j.StreamSrtPort = 50999
    $j | ConvertTo-Json -Depth 100 | Set-Content $prefsPath -Encoding UTF8
    $problems = @(Test-DrillPreferencesApplied -Path $prefsPath -Expected $script:patch.Expected)
    Assert ($problems.Count -eq 1 -and $problems[0] -match 'StreamSrtPort is a number, expected string') ($problems -join '; ')
    [IO.File]::WriteAllBytes($prefsPath, $saved)
  }

  # The app's own serializer, from the installed build (optional).
  $root = $InstallRoot
  if (-not $root -and -not $SkipAppSerializer) { try { $root = Resolve-DrillInstallRoot } catch { $root = $null } }
  $dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue).Source
  if ($SkipAppSerializer -or -not $root -or -not $dotnet -or -not (Test-Path (Join-Path $root 'CoreVideoPro.WinUI.dll'))) {
    $script:skipped += 'app serializer probe (needs dotnet and an installed CoreVideoPro.WinUI.dll)'
  } else {
    Check "the installed app's serializer loads the patched profile and rejects a numeric port ($root)" {
      $probeSrc = Join-Path $temp 'probe'; $null = New-Item -ItemType Directory -Path $probeSrc
      Copy-Item (Join-Path $PSScriptRoot 'outputs-drill-prefs-probe\*') $probeSrc
      $ErrorActionPreference = 'Continue'
      $build = & $dotnet build (Join-Path $probeSrc 'outputs-drill-prefs-probe.csproj') -c Release -nologo -v q -o (Join-Path $probeSrc 'out') 2>&1 | Out-String
      if ($LASTEXITCODE -ne 0) { throw "probe build failed: $build" }
      $numeric = Join-Path $temp 'numeric-port.json'
      $j = Get-Content $prefsPath -Raw -Encoding UTF8 | ConvertFrom-Json; $j.StreamSrtPort = 50999
      $j | ConvertTo-Json -Depth 100 | Set-Content $numeric -Encoding UTF8
      $lines = @(& $dotnet (Join-Path $probeSrc 'out\outputs-drill-prefs-probe.dll') $root $prefsPath $numeric 2>&1 | ForEach-Object { "$_" })
      if ($LASTEXITCODE -ne 0) { throw ($lines -join "`n") }
      $good = $lines[0] | ConvertFrom-Json; $bad = $lines[1] | ConvertFrom-Json
      Assert ($good.loaded -and $good.recordingTargetFolder -eq 'D:\CoreVideo-QA\drills\t\recordings' -and $good.streamRtmpEnabled -eq $false -and $good.streamSrtPort -eq '50999' -and $good.streamSrtHost -eq '127.0.0.1' -and $good.isoSourceCount -eq 1) "patched profile: $($lines[0])"
      Assert (-not $bad.loaded) "numeric port was accepted: $($lines[1])"
    }
  }

  Check 'patch with -Stream off leaves SRT alone but still forces RTMP off' {
    $other = Join-Path $temp 'nostream.json'; [IO.File]::WriteAllBytes($other, $originalBytes)
    $null = Set-DrillPreferences -Path $other -RecordingFolder 'D:\r' -IsoSourceIds @('zoom:1', 'zoom:2')
    $p = Get-Content $other -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert ($p.StreamRtmpEnabled -eq $false -and $p.StreamSrtMode -eq 'listener' -and $p.StreamSrtPort -ceq '50413') 'SRT touched'
    Assert (@($p.IsoRecordingSourceIds).Count -eq 2) 'iso'
  }
  Check 'patch adds missing keys on an older profile with the model types' {
    $old = Join-Path $temp 'old.json'
    [IO.File]::WriteAllText($old, '{"Version": 7}', [Text.UTF8Encoding]::new($false))
    $null = Set-DrillPreferences -Path $old -RecordingFolder 'D:\r' -IsoSourceIds @('zoom:1') -SrtPort 40000 -Stream
    $raw = [IO.File]::ReadAllText($old)
    Assert ($raw -match '"StreamSrtPort":\s*"40000"' -and $raw -match '"StreamRtmpEnabled":\s*false' -and $raw -match '"IsoRecordingSourceIds":\s*\[') $raw
  }
  Check 'restore puts back the primary and the .bak sidecar byte for byte' {
    [IO.File]::WriteAllText($sidecar, 'drill values the app saved as its sidecar')
    Restore-DrillPreferenceFiles -Manifest $script:manifest
    $a = [IO.File]::ReadAllBytes($prefsPath); $b = [IO.File]::ReadAllBytes($sidecar)
    Assert ($a.Length -eq $originalBytes.Length -and -not (Compare-Object $a $originalBytes -SyncWindow 0)) 'primary differs'
    Assert ($b.Length -eq $sidecarBytes.Length -and -not (Compare-Object $b $sidecarBytes -SyncWindow 0)) 'sidecar differs'
  }
  Check 'restore removes a sidecar that did not exist before the drill' {
    $dir2 = Join-Path $temp 'second'; $null = New-Item -ItemType Directory -Path $dir2
    $p2 = Join-Path $dir2 'production-output-preferences.json'; [IO.File]::WriteAllBytes($p2, $originalBytes)
    $m2 = Backup-DrillPreferenceFiles -Path $p2 -BackupDirectory (Join-Path $temp 'backup2')
    Assert (-not $m2[1].existed) 'sidecar reported as existing'
    [IO.File]::WriteAllText($p2 + '.bak', 'created by the app during the drill')
    Restore-DrillPreferenceFiles -Manifest $m2
    Assert (-not (Test-Path ($p2 + '.bak'))) 'drill-created sidecar left behind'
  }
  Check 'restore detects a corrupted backup' {
    [IO.File]::AppendAllText($script:manifest[0].backup, ' ')
    $threw = $false; try { Restore-DrillPreferenceFiles -Manifest $script:manifest } catch { $threw = $true }
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

# ---------------------------------------------------------------- readiness, invariants, throughout
function New-Envelope([switch]$Stale, [string]$Recording = 'producing', [int]$IsoFrames = 10, [int]$Sent = 5, [int]$Retry = 0,
                      [string]$Webcam = 'capture:f4978a7f6d614c0d', [long]$WebcamFrames = 100, [int]$WebcamWidth = 1920, [long]$ScreenFrames = -1,
                      $Fps = 60, [string[]]$Warnings = @(), [string]$Destination = 'srt', [string]$EncoderPath = 'hardware',
                      [bool]$ProgramHardware = $true, [string]$TargetFolder = 'D:\CoreVideo-QA\drills\t\recordings') {
  $adm = { param($id) [pscustomobject]@{ sourceId = $id; state = 'ready'; actualFrameId = 9; requestedFrameId = 9; actualEpoch = 1; requestedEpoch = 1; actualCapture100ns = 5; requestedCapture100ns = 5 } }
  [pscustomobject]@{ available = $true; ageMs = $(if ($Stale) { 2000 } else { 40 }); snapshot = [pscustomobject]@{
      graphics = [pscustomobject]@{ coreEpoch = 3 }
      recording = [pscustomobject]@{ targetFolder = $TargetFolder; lifecycle = [pscustomobject]@{ state = $Recording }
        encoder = [pscustomobject]@{ hardwareAccelerated = $ProgramHardware; hardwareRequested = $true; encoderPath = $(if ($ProgramHardware) { 'hardware' } else { 'software' }) }; streams = @(
          [pscustomobject]@{ kind = 'program'; framesWritten = 10 },
          [pscustomobject]@{ kind = 'iso'; sourceId = 'capture:f4978a7f6d614c0d'; readiness = 'ready'; framesWritten = $IsoFrames; encoderPath = $EncoderPath }) }
      sources = @([pscustomobject]@{ sourceId = $Webcam; hasVideo = $true; framesIngested = $WebcamFrames; width = $WebcamWidth; height = 1080 },
                  [pscustomobject]@{ sourceId = 'capture:screen:0'; hasVideo = $true; framesIngested = $(if ($ScreenFrames -ge 0) { $ScreenFrames } else { $WebcamFrames }); width = 2560; height = 1440 })
      programFrame = [pscustomobject]@{ health = 'live'; videoSources = @() }
      programSourceAdmission = [pscustomobject]@{ version = 1; sources = @((& $adm 'capture:screen:0'), (& $adm 'capture:f4978a7f6d614c0d')) }
      outputSenderSession = [pscustomobject]@{ activeSenderCount = 1; warnings = $Warnings
        senders = @([pscustomobject]@{ destination = $Destination; framesSent = $Sent; retryCount = $Retry; fps = $Fps }) }
      zoomSubscriptionChurn = [pscustomobject]@{ sources = @() } } }
}
$plan = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d' -RecordingFolder 'D:\CoreVideo-QA\drills\t\recordings'
Check 'readiness: a healthy local workload is ready' { $r = Test-DrillReadiness -Envelope (New-Envelope) -Plan $plan; Assert $r.Ready ($r.Reasons -join ';') }
Check 'readiness: stale snapshot, idle ISO, silent/slow/warning stream, missing webcam are not ready' {
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Stale) -Plan $plan).Ready) 'stale'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -IsoFrames 0) -Plan $plan).Ready) 'iso'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Sent 0) -Plan $plan).Ready) 'stream'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Fps 50) -Plan $plan).Ready) 'fps 50 accepted'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Warnings @('encoder spill')) -Plan $plan).Ready) 'warnings accepted'
  Assert (Test-DrillReadiness -Envelope (New-Envelope -Fps $null) -Plan $plan).Ready 'unreported fps refused'
  Assert (-not (Test-DrillReadiness -Envelope (New-Envelope -Webcam 'capture:0000000000000000') -Plan $plan).Ready) 'webcam'
}
Check 'readiness fatal: failed recording, stream retry, RTMP sender, recording outside the drill folder' {
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Recording failed) -Plan $plan).Fatal -eq 'Recording failed') 'failed'
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Retry 1) -Plan $plan).Fatal) 'retry'
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Destination 'rtmp') -Plan $plan).Fatal -match 'RTMP') 'rtmp'
  $noStream = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d' -Stream $false
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -Destination 'rtmp') -Plan $noStream).Fatal -match 'RTMP') 'rtmp with -SkipStream'
  Assert ((Test-DrillReadiness -Envelope (New-Envelope -TargetFolder 'C:\Users\someone\Videos') -Plan $plan).Fatal -match 'not applied') 'folder'
  Assert (Test-DrillReadiness -Envelope (New-Envelope -TargetFolder 'd:\corevideo-qa\drills\t\recordings\') -Plan $plan).Ready 'same folder, other case'
}
Check 'readiness: stream ignored when skipped' {
  $noStream = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d' -Stream $false
  Assert (Test-DrillReadiness -Envelope (New-Envelope -Sent 0) -Plan $noStream).Ready 'skipped stream still required'
}
Check 'readiness: a remote that is not live at 1080p blocks' {
  $remote = Get-DrillWorkloadPlan -RemoteIds @('77') -WebcamSourceId 'capture:f4978a7f6d614c0d'
  $e = New-Envelope
  $e.snapshot.zoomSubscriptionChurn.sources = @([pscustomobject]@{ kind = 'participant-video'; participantId = '77'; receivedWidth = 1280; receivedHeight = 720; coreFramesIngested = 9; lastCoreFrameAgeMs = 10 })
  Assert (-not (Test-DrillReadiness -Envelope $e -Plan $remote).Ready) '720p accepted'
}
Check 'invariants: healthy is null; core restart, stopped recording, changed Program, RTMP break' {
  Assert ($null -eq (Test-DrillInvariant -Envelope (New-Envelope) -Plan $plan -ExpectedEpoch 3)) 'healthy'
  Assert ((Test-DrillInvariant -Envelope (New-Envelope) -Plan $plan -ExpectedEpoch 4) -eq 'Native core restarted') 'epoch'
  Assert ((Test-DrillInvariant -Envelope (New-Envelope -Recording finalizing) -Plan $plan -ExpectedEpoch 3) -eq 'Recording stopped producing') 'recording'
  $e = New-Envelope; $e.snapshot.programSourceAdmission.sources = @($e.snapshot.programSourceAdmission.sources[0])
  Assert ((Test-DrillInvariant -Envelope $e -Plan $plan -ExpectedEpoch 3) -match 'Program source inventory changed') 'program'
  Assert ((Test-DrillInvariant -Envelope (New-Envelope -Destination 'rtmp') -Plan $plan -ExpectedEpoch 3) -match 'RTMP') 'rtmp'
}
Check 'throughout: a healthy interval keeps every flag true' {
  $t = New-DrillThroughout -Plan $plan; $progress = @{}
  for ($i = 0; $i -lt 5; $i++) { $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -WebcamFrames (100 + $i * 60)) -Plan $plan -Progress $progress -ElapsedMs ($i * 1000) }
  Assert ($t.samples -eq 5) "$($t.samples)"
  foreach ($k in 'hardwareEncodersThroughout', 'programEncoderHardwareThroughout', 'remotes1080Throughout', 'captureProgressThroughout', 'programSourcesAdmittedThroughout', 'streamHealthyThroughout') { Assert $t[$k] "$k went false" }
}
Check 'throughout: a frozen webcam fails capture progress after 2 s' {
  $t = New-DrillThroughout -Plan $plan; $progress = @{}
  foreach ($ms in 0, 1000, 1900) { $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -WebcamFrames 100) -Plan $plan -Progress $progress -ElapsedMs $ms }
  Assert $t.captureProgressThroughout 'failed before 2 s'
  $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -WebcamFrames 100) -Plan $plan -Progress $progress -ElapsedMs 2100
  Assert (-not $t.captureProgressThroughout) 'frozen webcam passed'
  Assert ($t.firstFailures[0].flag -eq 'captureProgressThroughout') 'first failure not recorded'
}
Check 'throughout: sub-1080 capture, 720p remote, software encoder, slow stream each fail their flag' {
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -WebcamWidth 1280) -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.captureProgressThroughout) '1280 webcam passed'
  $remote = Get-DrillWorkloadPlan -RemoteIds @('77') -WebcamSourceId 'capture:f4978a7f6d614c0d'
  $e = New-Envelope; $e.snapshot.zoomSubscriptionChurn.sources = @([pscustomobject]@{ kind = 'participant-video'; participantId = '77'; receivedWidth = 1280; receivedHeight = 720; coreFramesIngested = 9; lastCoreFrameAgeMs = 10 })
  $t = New-DrillThroughout -Plan $remote; $null = Update-DrillThroughout -Throughout $t -Envelope $e -Plan $remote -Progress @{} -ElapsedMs 0
  Assert (-not $t.remotes1080Throughout) '720p remote passed'
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -ProgramHardware $false) -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.hardwareEncodersThroughout) 'software Program not recorded as a fact'
  Assert (-not $t.programEncoderHardwareThroughout) 'software Program passed (D8: Program on software is a FAIL for RC)'
  Assert (@($t.firstFailures | Where-Object { $_.flag -eq 'programEncoderHardwareThroughout' }).Count -eq 1) 'software Program not a first failure'
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -Fps 40) -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.streamHealthyThroughout) 'slow stream passed'
}
Check 'throughout: a software ISO is a recorded fact, never a gating failure (#935, D8)' {
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -EncoderPath 'software') -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.hardwareEncodersThroughout) 'software ISO not recorded as a fact'
  Assert ((@($t.encoderPlacement['capture:f4978a7f6d614c0d'].paths) -join ',') -eq 'software') 'placement not recorded'
  Assert (-not $t.encoderPlacement['capture:f4978a7f6d614c0d'].changed) 'one sample reported as a change'
  foreach ($k in 'programEncoderHardwareThroughout', 'remotes1080Throughout', 'captureProgressThroughout', 'programSourcesAdmittedThroughout', 'streamHealthyThroughout') { Assert $t[$k] "$k failed on a placement-only difference" }
  Assert (@($t.firstFailures).Count -eq 0) ('placement became a first failure: ' + (@($t.firstFailures | ForEach-Object { $_.flag }) -join ','))
}
Check 'throughout: Program hardware is judged on the MEASURED encoderPath, never the request flag (#935 B1)' {
  $e = New-Envelope; $e.snapshot.recording.encoder = [pscustomobject]@{ hardwareAccelerated = $true; hardwareRequested = $true }
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope $e -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.programEncoderHardwareThroughout) 'unmeasured Program encoder passed as hardware'
  $e = New-Envelope; $e.snapshot.recording.encoder = [pscustomobject]@{ hardwareAccelerated = $false; hardwareRequested = $true; encoderPath = 'software' }
  $t = New-DrillThroughout -Plan $plan; $null = Update-DrillThroughout -Throughout $t -Envelope $e -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.programEncoderHardwareThroughout) 'hardware requested but software MFT measured passed'
}
Check 'throughout: an ISO whose encoder path changes between samples is reported as changed' {
  $t = New-DrillThroughout -Plan $plan
  $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope) -Plan $plan -Progress @{} -ElapsedMs 0
  Assert (-not $t.encoderPlacement['capture:f4978a7f6d614c0d'].changed) 'stable path reported as changed'
  $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -EncoderPath 'software') -Plan $plan -Progress @{} -ElapsedMs 1000
  Assert $t.encoderPlacement['capture:f4978a7f6d614c0d'].changed 'change not reported'
  Assert ((@($t.encoderPlacement['capture:f4978a7f6d614c0d'].paths) -join ',') -eq 'hardware,software') 'paths not accumulated'
}
Check 'throughout: an ISO that reports no path before its lazy open is a first observation, not a change' {
  $t = New-DrillThroughout -Plan $plan
  $e = New-Envelope
  foreach ($s in @($e.snapshot.recording.streams)) { if ($s.sourceId -eq 'capture:f4978a7f6d614c0d') { $s.encoderPath = $null } }
  $null = Update-DrillThroughout -Throughout $t -Envelope $e -Plan $plan -Progress @{} -ElapsedMs 0
  $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope) -Plan $plan -Progress @{} -ElapsedMs 1000
  $entry = $t.encoderPlacement['capture:f4978a7f6d614c0d']
  Assert (-not $entry.changed) 'lazy open reported as an encoder change'
  Assert ((@($entry.paths) -join ',') -eq 'hardware') "paths: $(@($entry.paths) -join ',')"
  Assert ($entry.notReportedSamples -eq 1) "notReportedSamples: $($entry.notReportedSamples)"
}
function New-CostStream([string]$Path = 'software', $Work = 9400000, $Mux = 1000, $Dropped = 0, $Index = 1) {
  [pscustomobject]@{ kind = 'iso'; sourceId = 'zoom:8'; isoIndex = $Index; encoderPath = $Path; videoWorkUs = $Work; muxVideoFrameCount = $Mux; droppedFrames = $Dropped; frameRate = 60 }
}
Check 'encoder cost: measured from deltas, absent is null, never zero' {
  $c = Get-DrillEncoderCost -PreviousCpuMs 1000 -CurrentCpuMs 1700 -PreviousWallMs 0 -CurrentWallMs 1000 -LogicalProcessors 28 `
    -PreviousStreams @(New-CostStream -Work 9400000 -Mux 1000) -CurrentStreams @(New-CostStream -Work 10000000 -Mux 1060 -Dropped 2)
  Assert ([math]::Abs($c.cpuPercent - 2.5) -lt 0.001) "cpu $($c.cpuPercent)"
  $sw = $c.software['zoom:8']
  Assert ($sw.isoIndex -eq 1 -and [math]::Abs($sw.meanVideoWorkUs - 10000000 / 1060) -lt 0.001) "mean $($sw.meanVideoWorkUs)"
  Assert ($sw.intervalVideoWorkUs -eq 10000) "interval $($sw.intervalVideoWorkUs)"
  Assert ($sw.droppedFrames -eq 2 -and $sw.frameRate -eq 60) 'dropped/fps'
  $none = Get-DrillEncoderCost -PreviousCpuMs $null -CurrentCpuMs 1700 -PreviousWallMs $null -CurrentWallMs 1000 -LogicalProcessors 28 `
    -PreviousStreams @() -CurrentStreams @(New-CostStream -Work 0 -Mux 0)
  Assert ($null -eq $none.cpuPercent) 'cpu without a previous sample must be null'
  Assert ($null -eq $none.software['zoom:8'].meanVideoWorkUs) 'zero frames gave a mean'
  Assert ($null -eq $none.software['zoom:8'].intervalVideoWorkUs) 'no previous sample gave an interval cost'
  $missing = Get-DrillEncoderCost -PreviousCpuMs 1 -CurrentCpuMs 2 -PreviousWallMs 0 -CurrentWallMs 1000 -LogicalProcessors 28 `
    -PreviousStreams @() -CurrentStreams @([pscustomobject]@{ kind = 'iso'; sourceId = 'zoom:8'; encoderPath = 'software' })
  Assert ($null -eq $missing.software['zoom:8'].meanVideoWorkUs -and $null -eq $missing.software['zoom:8'].droppedFrames) 'absent counters became numbers'
  $hw = Get-DrillEncoderCost -PreviousCpuMs 1 -CurrentCpuMs 2 -PreviousWallMs 0 -CurrentWallMs 1000 -LogicalProcessors 28 -PreviousStreams @() -CurrentStreams @(New-CostStream -Path 'hardware')
  Assert ($hw.software.Count -eq 0) 'hardware ISO costed as software'
}
Check 'encoder cost record: mean and peak over intervals, start and end drops' {
  $acc = New-DrillEncoderCostRecord -LogicalProcessors 28
  Add-DrillEncoderCost -Record $acc -Interval ([pscustomobject]@{ cpuPercent = $null; software = @{ 'zoom:8' = [ordered]@{ isoIndex = 8; meanVideoWorkUs = $null; intervalVideoWorkUs = $null; droppedFrames = 0; frameRate = 60 } } })
  Add-DrillEncoderCost -Record $acc -Interval ([pscustomobject]@{ cpuPercent = 20.0; software = @{ 'zoom:8' = [ordered]@{ isoIndex = 8; meanVideoWorkUs = 9000.0; intervalVideoWorkUs = 9000.0; droppedFrames = 0; frameRate = 60 } } })
  Add-DrillEncoderCost -Record $acc -Interval ([pscustomobject]@{ cpuPercent = 40.0; software = @{ 'zoom:8' = [ordered]@{ isoIndex = 8; meanVideoWorkUs = 9500.0; intervalVideoWorkUs = 14000.0; droppedFrames = 1; frameRate = 60 } } })
  $r = ConvertTo-DrillEncoderCostJson -Record $acc
  Assert ($r.samples -eq 3 -and $r.cpuSamples -eq 2 -and $r.logicalProcessors -eq 28) 'counts'
  Assert ($r.coreCpuPercent.mean -eq 30.0 -and $r.coreCpuPercent.peak -eq 40.0) "cpu $($r.coreCpuPercent | ConvertTo-Json -Compress)"
  $sw = $r.software['zoom:8']
  Assert ($sw.isoIndex -eq 8 -and $sw.meanVideoWorkUs -eq 9500.0 -and $sw.peakIntervalVideoWorkUs -eq 14000.0) 'software cost'
  Assert ($sw.droppedFramesStart -eq 0 -and $sw.droppedFramesEnd -eq 1 -and $sw.frameRate -eq 60) 'drops'
  $empty = ConvertTo-DrillEncoderCostJson -Record (New-DrillEncoderCostRecord -LogicalProcessors 28)
  Assert ($null -eq $empty.coreCpuPercent.mean -and $null -eq $empty.coreCpuPercent.peak) 'no CPU samples must be null, not zero'
}
Check 'parameters: -ProbeMaxSessions is optional and limited to 1..16' {
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -ProbeMaxSessions 0).Count -eq 0) 'unset (0) refused'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -ProbeMaxSessions 8).Count -eq 0) '8 refused'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -ProbeMaxSessions 17).Count -eq 1) '17 accepted'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -ProbeMaxSessions -1).Count -eq 1) '-1 accepted'
}
Check 'throughout: samples outside the interval only refresh progress' {
  $t = New-DrillThroughout -Plan $plan
  $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -WebcamWidth 1280) -Plan $plan -Progress @{} -ElapsedMs 0 -Measuring $false
  Assert ($t.samples -eq 0 -and $t.captureProgressThroughout) 'settle sample counted'
}
Check 'sample row is compact, carries the QPC and the admission evidence the judge needs' {
  $s = Update-DrillThroughout -Throughout (New-DrillThroughout -Plan $plan) -Envelope (New-Envelope) -Plan $plan -Progress @{} -ElapsedMs 0
  $row = Get-DrillSampleRow -Envelope (New-Envelope) -ElapsedMs 1.5 -Checks $s.Checks -Captures $s.Captures
  Assert ($row.qpc -match '^[0-9]+$' -and $row.coreEpoch -eq 3 -and @($row.iso).Count -eq 2) 'row'
  $json = $row | ConvertTo-Json -Depth 12 -Compress | ConvertFrom-Json
  Assert ($json.snapshot.programSourceAdmission.version -eq 1 -and @($json.snapshot.programSourceAdmission.sources).Count -eq 2) 'admissions missing from row'
  Assert ($json.checks.captureProgressThroughout -eq $true -and @($json.captures).Count -eq 2) 'checks/captures missing'
}

# ---------------------------------------------------------------- close guard
Check 'safe to close only when nothing is recording, streaming or finalizing and the engine is off' {
  $idle = [pscustomobject]@{ recording = [pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'completed'; finalized = $true } }
    outputSenderSession = [pscustomobject]@{ activeSenderCount = 0; senders = @([pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'completed' } }) } }
  $off = [pscustomobject]@{ recording = $false; streaming = $false; engineOn = $false }
  Assert (@(Test-DrillSafeToClose $off $idle).Count -eq 0) (@(Test-DrillSafeToClose $off $idle) -join ';')
  Assert (@(Test-DrillSafeToClose ([pscustomobject]@{ recording = $true; streaming = $false; engineOn = $false }) $idle).Count -eq 1) 'recording flag ignored'
  Assert (@(Test-DrillSafeToClose ([pscustomobject]@{ recording = $false; streaming = $false; engineOn = $true }) $idle).Count -eq 1) 'engine ignored'
  $finalizing = [pscustomobject]@{ recording = [pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'finalizing' } }; outputSenderSession = $idle.outputSenderSession }
  Assert (@(Test-DrillSafeToClose $off $finalizing) -match 'finalizing').Count 'finalizing ignored'
  $stopping = [pscustomobject]@{ recording = $idle.recording; outputSenderSession = [pscustomobject]@{ activeSenderCount = 1; senders = @([pscustomobject]@{ lifecycle = [pscustomobject]@{ state = 'stopping' } }) } }
  Assert (@(Test-DrillSafeToClose $off $stopping).Count -eq 2) 'stopping sender ignored'
  Assert (@(Test-DrillSafeToClose $null $idle).Count -eq 1) 'missing state treated as safe'
  $never = [pscustomobject]@{ recording = [pscustomobject]@{ lifecycle = $null }; outputSenderSession = [pscustomobject]@{ activeSenderCount = 0; senders = @() } }
  Assert (@(Test-DrillSafeToClose $off $never).Count -eq 0) 'never-started outputs blocked'
}

# ---------------------------------------------------------------- 2026-10-25 shakeout defects
Check 'a one-ISO local plan keeps IsoSourceIds an array through JSON and needs no Zoom engine' {
  $p = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d'
  Assert ($p.IsoSourceIds -is [array]) 'IsoSourceIds unrolled to a scalar'
  Assert ($p.IsoSourceIds.Count -eq 1 -and $p.IsoSourceIds[0] -eq 'capture:f4978a7f6d614c0d') 'wrong ISO list'
  $json = [ordered]@{ program = $true; iso = @($p.IsoSourceIds) } | ConvertTo-Json -Compress
  Assert ($json.Contains('"iso":["capture:f4978a7f6d614c0d"]')) "drill.json would carry a string: $json"
  Assert (-not $p.EngineRequired) 'local-only plan must not call transport.engine.set (refused with 422 outside a meeting)'
  $remote = Get-DrillWorkloadPlan -RemoteIds @('77') -WebcamSourceId 'capture:f4978a7f6d614c0d'
  Assert ($remote.EngineRequired) 'remote plan must turn the engine on'
}

Check 'a webcam missing from the capture-device list is refused before any output starts' {
  $devices = @([pscustomobject]@{ id = 'ddc344995cb38b42'; name = 'NDI Webcam Video 1'; vendor = 'uvc' }, [pscustomobject]@{ id = 'screen:0'; name = 'DISPLAY1'; vendor = 'Windows Graphics Capture' })
  $r = @(Test-DrillWebcamPresent -WebcamSourceId 'capture:f4978a7f6d614c0d' -Devices $devices)
  Assert ($r.Count -eq 1 -and $r[0] -match 'not in the app' -and $r[0] -match 'NDI Webcam Video 1') "unexpected: $r"
  Assert (@(Test-DrillWebcamPresent -WebcamSourceId 'capture:ddc344995cb38b42' -Devices $devices).Count -eq 0) 'present webcam refused'
  Assert (@(Test-DrillWebcamPresent -WebcamSourceId 'capture:x' -Devices @()).Count -eq 1) 'empty device list accepted'
}

# ---------------------------------------------------------------- 2026-10-26: webcam optional (camera rig off the bench)
Check '-NoWebcam local plan: screen only in Program, empty webcam route, no ISO, reported not exercised' {
  $p = Get-DrillWorkloadPlan -WebcamSourceId ''
  Assert ($p.LocalOnly -and -not $p.WebcamExercised) 'plan still thinks the webcam is exercised'
  Assert ((($p.Assignments | ForEach-Object { "$($_.slot)=$($_.source)" }) -join ',') -eq '6=capture:screen:0') 'webcam slot assigned'
  Assert ($p.Routes -eq '{"speaker-slides-1":null,"speaker-slides-2":6}') $p.Routes
  Assert (($p.ProgramSourceIds -join '|') -eq 'capture:screen:0') ($p.ProgramSourceIds -join '|')
  Assert (($p.CaptureSourceIds -join '|') -eq 'capture:screen:0') ($p.CaptureSourceIds -join '|')
  Assert ($p.IsoSourceIds -is [array] -and $p.IsoSourceIds.Count -eq 0) 'a -NoWebcam local plan must have no ISO'
  Assert ($p.Workload -match 'webcam not exercised' -and $p.Workload -match 'no ISO') $p.Workload
  Assert (-not $p.EngineRequired) 'engine'
  $json = [ordered]@{ program = $true; iso = @($p.IsoSourceIds) } | ConvertTo-Json -Compress
  Assert ($json.Contains('"iso":[]')) "drill.json would not carry an empty ISO array: $json"
}
Check '-NoWebcam remote plan keeps the Zoom ISOs and drops only the webcam' {
  $p = Get-DrillWorkloadPlan -RemoteIds @('11', '22') -WebcamSourceId ''
  Assert ((($p.Assignments | ForEach-Object { "$($_.slot)=$($_.source)" }) -join ',') -eq '1=zoom:11,2=zoom:22,6=capture:screen:0') 'slots'
  Assert (($p.IsoSourceIds -join '|') -eq 'zoom:11|zoom:22') 'iso'
  Assert ($p.EngineRequired -and -not $p.WebcamExercised) 'engine / exercised'
  Assert ($p.Workload -match '2 remote Zoom feed\(s\)' -and $p.Workload -match 'webcam not exercised') $p.Workload
}
Check '-NoWebcam and -WebcamSourceId exclude each other; the plan with a webcam is unchanged' {
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -NoWebcam $true).Count -eq 0) 'NoWebcam alone refused'
  Assert ((Test-DrillParameters -Seconds 60 -Out 'D:\x' -NoWebcam $true -WebcamSourceId 'capture:f4978a7f6d614c0d').Count -eq 1) 'both accepted'
  $p = Get-DrillWorkloadPlan -WebcamSourceId 'capture:f4978a7f6d614c0d'
  Assert ($p.WebcamExercised -and $p.Routes -eq '{"speaker-slides-1":10,"speaker-slides-2":6}') 'webcam plan changed'
}
Check '-NoWebcam readiness and throughout judge the screen alone; a missing webcam no longer blocks' {
  $noCam = Get-DrillWorkloadPlan -WebcamSourceId '' -RecordingFolder 'D:\CoreVideo-QA\drills\t\recordings'
  $envelope = New-Envelope -Webcam 'capture:0000000000000000'
  # The envelope's Program admissions still name the webcam, so fix them to the screen-only inventory.
  $envelope.snapshot.programSourceAdmission.sources = @($envelope.snapshot.programSourceAdmission.sources | Where-Object { $_.sourceId -eq 'capture:screen:0' })
  $envelope.snapshot.recording.streams = @($envelope.snapshot.recording.streams | Where-Object { $_.kind -ne 'iso' })
  $r = Test-DrillReadiness -Envelope $envelope -Plan $noCam
  Assert $r.Ready ("not ready: " + ($r.Reasons -join '; '))
  $t = New-DrillThroughout -Plan $noCam; $progress = @{}
  for ($i = 0; $i -lt 5; $i++) { $null = Update-DrillThroughout -Throughout $t -Envelope (New-Envelope -Webcam 'capture:0000000000000000' -ScreenFrames (100 + $i * 60)) -Plan $noCam -Progress $progress -ElapsedMs ($i * 1000) }
  Assert $t.captureProgressThroughout 'screen progress failed without a webcam'
  Assert $t.hardwareEncodersThroughout 'no ISO must not fail the hardware fact'
}
Check 'patch with no ISO source turns ISO recording off instead of arming an empty list' {
  $noIso = Join-Path $temp 'no-iso\production-output-preferences.json'
  $null = New-Item -ItemType Directory -Path (Split-Path $noIso) -Force
  [IO.File]::WriteAllText($noIso, $v14, [Text.UTF8Encoding]::new($false))
  $null = Set-DrillPreferences -Path $noIso -RecordingFolder 'D:\r' -IsoSourceIds @() -SrtPort 40000 -Stream
  $p = Get-Content $noIso -Raw -Encoding UTF8 | ConvertFrom-Json
  Assert ($p.IsoRecordingEnabled -eq $false) 'ISO armed with no sources'
  Assert (@($p.IsoRecordingSourceIds).Count -eq 0) 'ISO list not emptied'
  Assert ([IO.File]::ReadAllText($noIso) -match '"IsoRecordingSourceIds":\s*\[\s*\]') 'ISO list must stay a JSON array'
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

foreach ($s in $script:skipped) { Write-Host "SKIP  $s" -ForegroundColor Yellow }
Write-Host "`n$($script:cases - $script:failures)/$($script:cases) passed$(if ($script:skipped.Count) { ", $($script:skipped.Count) skipped" })"
if ($script:failures) { exit 1 }
exit 0
