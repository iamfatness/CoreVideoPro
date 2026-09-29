param(
    [ValidateRange(10, 3600)][int]$Seconds = 120,
    [string]$Adapter = 'Ethernet',
    [string]$Gateway = '192.168.1.1',
    [string]$InternetProbe = '1.1.1.1',
    [string]$OutputDirectory = 'artifacts/network-qa',
    [switch]$StartStream,
    [switch]$StopOnOutputStall
)

# Windows, whole-adapter and whole-host TCP counters, NOT per-socket telemetry.
# StartStream uses the operator's configured destination and stops it on exit.
# Without StartStream this is read-only, including when the watchdog trips.
$ErrorActionPreference = 'Stop'
$api = 'http://127.0.0.1:8011'
$nic = [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
    Where-Object Name -eq $Adapter | Select-Object -First 1
if (!$nic) { throw "Network adapter '$Adapter' was not found." }
$state = Invoke-RestMethod "$api/state" -TimeoutSec 5
if ($StartStream -and $state.streaming) { throw 'Stop the existing stream before an owned test run.' }
if ($StopOnOutputStall -and !$StartStream) { throw '-StopOnOutputStall requires -StartStream ownership.' }
$null = New-Item -ItemType Directory -Force $OutputDirectory
$run = Join-Path $OutputDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$runStartedUtc = [DateTime]::UtcNow
$ping = New-Object System.Net.NetworkInformation.Ping
$owned = $false
$bad = 0
$previousOutput = $null
$slowOutputWindows = 0
try {
    if ($StartStream) {
        # Set ownership before sending: a timed-out HTTP response may still start it.
        $owned = $true
        $result = Invoke-RestMethod "$api/invoke" -Method Post -ContentType application/json `
            -Body '{"action":"transport.stream.set","args":[true]}' -TimeoutSec 10
        if (!$result.ok) { throw "Stream start failed: $($result.error)" }
    }
    $previous = $nic.GetIPv4Statistics()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $previousMs = 0
    $sample = 0
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 900
        $lan = $ping.Send($Gateway, 350)
        $wan = $ping.Send($InternetProbe, 350)
        $now = $timer.ElapsedMilliseconds
        $stats = $nic.GetIPv4Statistics()
        $tcp = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetTcpIPv4Statistics()
        $dt = ($now - $previousMs) / 1000.0
        $row = [pscustomobject]@{
            utc = [DateTime]::UtcNow.ToString('o')
            txMbps = ($stats.BytesSent - $previous.BytesSent) * 8 / 1e6 / $dt
            rxMbps = ($stats.BytesReceived - $previous.BytesReceived) * 8 / 1e6 / $dt
            txPacketsPerSecond = ($stats.UnicastPacketsSent - $previous.UnicastPacketsSent) / $dt
            tcpRetransmitted = $tcp.SegmentsResent
            gatewayStatus = "$($lan.Status)"; gatewayMs = $lan.RoundtripTime
            internetStatus = "$($wan.Status)"; internetMs = $wan.RoundtripTime
        }
        $row | ConvertTo-Json -Compress | Add-Content "$run-network.jsonl"
        $previous = $stats; $previousMs = $now
        if (($sample++ % 10) -eq 0) {
            Invoke-RestMethod "$api/snapshot" -TimeoutSec 5 |
                ConvertTo-Json -Depth 60 -Compress | Add-Content "$run-snapshots.jsonl"
            if ($StopOnOutputStall) {
                $live = Invoke-RestMethod "$api/state" -TimeoutSec 5
                $sender = @($live.nativeObservation.outputSenderSession.senders |
                    Where-Object destination -eq 'rtmp' | Select-Object -First 1)[0]
                $ffmpegLog = Get-ChildItem $env:TEMP -Filter 'corevideo-ffmpeg-rtmp-*.log' -File |
                    Where-Object CreationTimeUtc -ge $runStartedUtc.AddSeconds(-2) |
                    Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
                $progress = if ($ffmpegLog) {
                    Get-Content $ffmpegLog.FullName -Tail 5 |
                        Where-Object { $_ -match 'frame=\s*\d+.*elapsed=' } | Select-Object -Last 1
                }
                $ffmpegFrames = if ($progress -match 'frame=\s*(\d+)') { [int64]$Matches[1] } else { $null }
                $ffmpegSizeKiB = if ($progress -match 'size=\s*(\d+)KiB') {
                    [int64]$Matches[1]
                } else { $null }
                $output = [pscustomobject]@{
                    utc = [DateTime]::UtcNow.ToString('o')
                    elapsedSeconds = $timer.Elapsed.TotalSeconds
                    streaming = $live.streaming
                    senderStatus = if ($sender) { $sender.status } else { 'absent' }
                    senderFrames = if ($sender) { $sender.framesSent } else { 0 }
                    # The sender's sendBytesWritten currently counts flushed proof JSON,
                    # not encoded pipe bytes. Do not mistake it for transport throughput.
                    proofBytes = if ($sender) { $sender.sendBytesWritten } else { 0 }
                    ffmpegFrames = $ffmpegFrames
                    ffmpegSizeKiB = $ffmpegSizeKiB
                    backpressureEntries = if ($sender) { $sender.backpressure.enteredCount } else { 0 }
                    backpressureBufferedMs = if ($sender) { $sender.backpressure.bufferedMs } else { 0 }
                    programDelivered = $live.nativeProgramBuffer.delivered
                    programProduced = $live.nativeProgramBuffer.produced
                    encoderShedFrames = $live.nativeObservation.realtimeEvidence.encoderExport.shedFrames
                }
                $output | ConvertTo-Json -Compress | Add-Content "$run-output.jsonl"
                if ($timer.Elapsed.TotalSeconds -gt 20) {
                    if ($null -eq $output.ffmpegFrames) {
                        throw 'Output watchdog: FFmpeg frame progress is unavailable.'
                    }
                    if (!$live.streaming -or !$sender -or $sender.status -ne 'live') {
                        throw 'Output watchdog: RTMP sender is no longer live.'
                    }
                    if ($output.backpressureEntries -gt 0 -or $output.encoderShedFrames -gt 0) {
                        throw 'Output watchdog: sender backpressure or encoder frame shedding began.'
                    }
                    if ($previousOutput) {
                        $period = $output.elapsedSeconds - $previousOutput.elapsedSeconds
                        $senderFps = ($output.senderFrames - $previousOutput.senderFrames) / $period
                        $programFps = ($output.programDelivered - $previousOutput.programDelivered) / $period
                        $ffmpegFps = if ($null -ne $output.ffmpegFrames -and
                            $null -ne $previousOutput.ffmpegFrames) {
                            ($output.ffmpegFrames - $previousOutput.ffmpegFrames) / $period
                        } else { 60 }
                        if ($senderFps -lt 45 -or $programFps -lt 45 -or $ffmpegFps -lt 45) {
                            $slowOutputWindows++
                        } else {
                            $slowOutputWindows = 0
                        }
                        if ($slowOutputWindows -ge 2) {
                            throw "Output watchdog: two slow windows (sender=$([math]::Round($senderFps, 1)) fps, Program=$([math]::Round($programFps, 1)) fps, FFmpeg=$([math]::Round($ffmpegFps, 1)) fps)."
                        }
                    }
                }
                $previousOutput = $output
            }
            $row | ConvertTo-Json -Compress | Write-Output
        }
        if ($lan.Status -ne 'Success' -or $lan.RoundtripTime -gt 100 -or
            $wan.Status -ne 'Success' -or $wan.RoundtripTime -gt 250) { $bad++ } else { $bad = 0 }
        if ($bad -ge 3) { throw 'Network watchdog: three consecutive degraded samples; ending test.' }
    }
} finally {
    if ($owned) {
        $result = Invoke-RestMethod "$api/invoke" -Method Post -ContentType application/json `
            -Body '{"action":"transport.stream.set","args":[false]}' -TimeoutSec 10
        if (!$result.ok) { throw "Stream stop failed; check the app: $($result.error)" }
    }
    $ping.Dispose()
    Write-Output "Evidence prefix: $run"
}
