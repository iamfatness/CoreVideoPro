# Simple integration smoke test for the media-core stdio bridge.
# Requires corevideo-native.exe; the old Electron/Node core stub has been removed.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

$nativeCandidates = @(
    (Join-Path $repoRoot "native\build-dev\corevideo-native.exe"),
    (Join-Path $repoRoot "native\build\corevideo-native.exe"),
    (Join-Path $repoRoot "native\build-dev\Release\corevideo-native.exe")
)
$nativeExe = $nativeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($nativeExe) {
    $runner = $nativeExe
    $runnerArgs = ""
    Write-Host "Using packaged native core: $runner"
}
else {
    Write-Error "corevideo-native.exe is required. Run npm run test:native-media-core or scripts/build-studio.ps1 first."
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $runner
$psi.Arguments = $runnerArgs
$psi.WorkingDirectory = $repoRoot
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$process = [System.Diagnostics.Process]::Start($psi)
if (-not $process) {
    Write-Error "Failed to start media core process."
}

function Send-Line($json) {
    $request = $json | ConvertFrom-Json
    $process.StandardInput.WriteLine($json)
    $process.StandardInput.Flush()
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $read = $process.StandardOutput.ReadLineAsync()
    while ($true) {
        if ([DateTime]::UtcNow -gt $deadline) {
            throw "Timed out waiting for response to $($request.id)."
        }
        if ($process.HasExited) {
            $stderr = $process.StandardError.ReadToEnd()
            throw "Media core process exited with code $($process.ExitCode) before responding to $($request.id). $stderr"
        }
        if (-not $read.Wait(250)) {
            continue
        }
        $line = $read.Result
        if (-not $line) {
            $read = $process.StandardOutput.ReadLineAsync()
            continue
        }
        $payload = $line | ConvertFrom-Json
        if ($request.type -eq "handshake" -and $payload.type -eq "handshake" -and $payload.ok) {
            return $line
        }
        if ($payload.id -eq $request.id) {
            return $line
        }
        $read = $process.StandardOutput.ReadLineAsync()
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
$expectedBaselineMeetingState = "idle"
if ($sync.snapshot.meetingState -ne $expectedBaselineMeetingState) {
    throw "Expected baseline meetingState '$expectedBaselineMeetingState', got '$($sync.snapshot.meetingState)'."
}
Write-Host "Baseline breakout room ok: $($sync.snapshot.breakoutRoomName) (meetingState=$($sync.snapshot.meetingState))"

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

$joinPayload = @{
    id = "core-5"
    type = "zoom-join"
    payload = @{
        meetingUrl = "https://zoom.us/j/123456789"
        displayName = "Operator"
        webinar = $true
    }
} | ConvertTo-Json -Depth 6 -Compress

$join = Send-Line $joinPayload | ConvertFrom-Json
if (-not $join.ok) { throw "zoom-join failed." }
if ($join.snapshot.meetingState -ne "in_meeting") {
    throw "Expected zoom-join meetingState 'in_meeting', got '$($join.snapshot.meetingState)'."
}
if ($join.snapshot.participants.Count -lt 1) {
    throw "Expected zoom-join participants in snapshot."
}
Write-Host "zoom-join ok: $($join.snapshot.participants.Count) participants"

$spineParticipants = @(
    @{
        sdkUserId = "operator-1"
        displayName = "Operator"
        role = "guest"
        videoOn = $true
        muted = $false
        talking = $true
        sharingScreen = $false
        audioLevel = 70
        networkQuality = "good"
    }
)
$spinePayload = @{
    id = "core-5b"
    type = "zoom-media-spine-sync"
    elapsedMs = 1500
    spinePayload = @{
        readiness = @{
            status = "ready"
            platform = "windows"
            sdkVersion = "zoom-engine"
            checks = @()
            blockers = @()
            warnings = @()
            summary = "ready"
        }
        participants = $spineParticipants
        subscriptions = @(
            @{
                participantId = "operator-1"
                kind = "participant-video"
                purpose = "active-speaker"
                priority = 10
            }
        )
        blocked = $false
        warnings = @()
        summary = "1 Zoom participant, 1 raw subscriptions requested."
    }
} | ConvertTo-Json -Depth 8 -Compress

$spine = Send-Line $spinePayload | ConvertFrom-Json
if (-not $spine.ok) { throw "zoom-media-spine-sync failed." }
if ($spine.type -ne "zoom-media-spine-sync") {
    throw "Expected zoom-media-spine-sync response type, got '$($spine.type)'."
}
if (-not $spine.spineSnapshot) {
    throw "Expected zoom-media-spine-sync spineSnapshot payload."
}
if ($spine.spineSnapshot.participantCount -lt 1) {
    throw "Expected spine snapshot participantCount >= 1."
}
Write-Host "zoom-media-spine-sync ok: $($spine.spineSnapshot.participantCount) participants"

$zoomSnapshot = Send-Line '{"id":"core-6","type":"zoom-snapshot"}' | ConvertFrom-Json
if (-not $zoomSnapshot.ok) { throw "zoom-snapshot failed." }
if ($zoomSnapshot.snapshot.meetingState -ne "in_meeting") {
    throw "Expected zoom-snapshot meetingState 'in_meeting', got '$($zoomSnapshot.snapshot.meetingState)'."
}
$operator = $zoomSnapshot.snapshot.participants | Where-Object { $_.userId -eq "operator-1" } | Select-Object -First 1
if (-not $operator) {
    throw "Expected zoom-snapshot roster to include participant id 'operator-1'."
}
Write-Host "zoom-snapshot ok: $($zoomSnapshot.snapshot.participants.Count) participants (operator-1 present)"

$syncAfterJoinPayload = @{
    id = "core-7"
    type = "media-core-sync"
    elapsedMs = 3000
    commands = @(
        @{
            type = "load-scene-graph"
            sceneId = "speaker-slides"
            routes = @(
                @{
                    routeId = "program"
                    mode = "active-speaker"
                    audioRole = "mix"
                    participantId = "operator-1"
                }
            )
        }
    )
} | ConvertTo-Json -Depth 6 -Compress

$syncAfterJoin = Send-Line $syncAfterJoinPayload | ConvertFrom-Json
if (-not $syncAfterJoin.ok) { throw "post-join media-core-sync failed." }
if ($syncAfterJoin.snapshot.meetingState -ne "in_meeting") {
    throw "Expected post-join meetingState 'in_meeting', got '$($syncAfterJoin.snapshot.meetingState)'."
}
if ($syncAfterJoin.snapshot.participants.Count -lt 1) {
    throw "Expected roster participants on post-join sync snapshot."
}
$postJoinOperator = $syncAfterJoin.snapshot.participants | Where-Object { $_.userId -eq "operator-1" } | Select-Object -First 1
if (-not $postJoinOperator) {
    throw "Expected post-join sync roster to include participant id 'operator-1'."
}
Write-Host "Post-join roster ok: $($syncAfterJoin.snapshot.participants.Count) participants (operator-1 present)"

$leave = Send-Line '{"id":"core-8","type":"zoom-leave"}' | ConvertFrom-Json
if (-not $leave.ok) { throw "zoom-leave failed." }
if ($leave.snapshot.meetingState -ne "idle") {
    throw "Expected zoom-leave meetingState 'idle', got '$($leave.snapshot.meetingState)'."
}
Write-Host "zoom-leave ok"

$process.Kill()
$process.WaitForExit()
Write-Host "Integration smoke test passed."
