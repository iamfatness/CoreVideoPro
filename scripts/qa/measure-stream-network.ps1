param(
    [ValidateRange(10, 3600)][int]$Seconds = 120,
    [string]$Adapter = 'Ethernet',
    [string]$Gateway = '192.168.1.1',
    [string]$InternetProbe = '1.1.1.1',
    [string]$OutputDirectory = 'artifacts/network-qa',
    [switch]$StartStream
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
$null = New-Item -ItemType Directory -Force $OutputDirectory
$run = Join-Path $OutputDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$ping = New-Object System.Net.NetworkInformation.Ping
$owned = $false
$bad = 0
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
