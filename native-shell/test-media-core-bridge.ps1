# Simple integration smoke test for the media-core stdio bridge.
# Requires Node and desktop/coreStub.cjs (or a built corevideo-native.exe).

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$stub = Join-Path $repoRoot "desktop\coreStub.cjs"

if (-not (Test-Path $stub)) {
    Write-Error "coreStub.cjs not found at $stub"
}

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    $electron = Join-Path $repoRoot "node_modules\electron\dist\electron.exe"
    if (Test-Path $electron) {
        $env:ELECTRON_RUN_AS_NODE = "1"
        $runner = $electron
    }
    else {
        Write-Error "Node.js is required to run the integration script."
    }
}
else {
    $runner = $node.Source
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $runner
$psi.Arguments = "`"$stub`""
$psi.WorkingDirectory = $repoRoot
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$process = [System.Diagnostics.Process]::Start($psi)
if (-not $process) {
    Write-Error "Failed to start media core stub."
}

function Send-Line($json) {
    $request = $json | ConvertFrom-Json
    $process.StandardInput.WriteLine($json)
    $process.StandardInput.Flush()
    while ($true) {
        $line = $process.StandardOutput.ReadLine()
        if (-not $line) {
            throw "Media core process closed stdout before responding to $($request.id)."
        }
        $payload = $line | ConvertFrom-Json
        if ($payload.id -eq $request.id) {
            return $line
        }
    }
}

$handshake = Send-Line '{"id":"core-1","type":"handshake"}' | ConvertFrom-Json
if (-not $handshake.ok) { throw "Handshake failed." }
Write-Host "Handshake ok: $($handshake.profile.name)"

$ping = Send-Line '{"id":"core-2","type":"ping"}' | ConvertFrom-Json
if (-not $ping.ok) { throw "Ping failed." }
Write-Host "Ping ok"

$syncPayload = @{
    id = "core-3"
    type = "media-core-sync"
    elapsedMs = 1000
    commands = @(
        @{
            type = "load-scene-graph"
            sceneId = "speaker-slides"
            routes = @(
                @{
                    routeId = "program"
                    mode = "active-speaker"
                    audioRole = "mix"
                    participantId = "p2"
                }
            )
        }
    )
} | ConvertTo-Json -Depth 6 -Compress

$sync = Send-Line $syncPayload | ConvertFrom-Json
if (-not $sync.ok) { throw "media-core-sync failed." }
Write-Host "media-core-sync ok: scene=$($sync.snapshot.sceneId) frames=$($sync.snapshot.programFrameCount)"

if ($sync.snapshot.breakoutRoomId -ne "main") {
    throw "Expected default breakoutRoomId 'main', got '$($sync.snapshot.breakoutRoomId)'."
}
if ($sync.snapshot.meetingState -ne "in_meeting") {
    throw "Expected meetingState 'in_meeting', got '$($sync.snapshot.meetingState)'."
}
Write-Host "Baseline breakout room ok: $($sync.snapshot.breakoutRoomName)"

$roomChangePayload = @{
    id = "core-4"
    type = "media-core-sync"
    elapsedMs = 2000
    commands = @(
        @{
            type = "simulate-breakout-room-change"
            breakoutRoomId = "customer-panel"
            breakoutRoomName = "Customer panel"
        },
        @{
            type = "load-scene-graph"
            sceneId = "speaker-slides"
            routes = @(
                @{
                    routeId = "program"
                    mode = "active-speaker"
                    audioRole = "mix"
                    participantId = "p2"
                }
            )
        }
    )
} | ConvertTo-Json -Depth 6 -Compress

$roomChange = Send-Line $roomChangePayload | ConvertFrom-Json
if (-not $roomChange.ok) { throw "breakout-room-change sync failed." }
if ($roomChange.snapshot.breakoutRoomId -ne "customer-panel") {
    throw "Expected breakoutRoomId 'customer-panel', got '$($roomChange.snapshot.breakoutRoomId)'."
}
if ($roomChange.snapshot.breakoutRoomName -ne "Customer panel") {
    throw "Expected breakoutRoomName 'Customer panel', got '$($roomChange.snapshot.breakoutRoomName)'."
}
Write-Host "Breakout room change ok: $($roomChange.snapshot.breakoutRoomName)"

$process.Kill()
$process.WaitForExit()
Write-Host "Integration smoke test passed."