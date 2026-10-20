param(
    [Parameter(Mandatory=$true)][string]$NativeCore,
    [Parameter(Mandatory=$true)][string]$Pattern,
    [string]$OutputDirectory = 'artifacts/engine-host-960/drill',
    [int]$Port = 19379
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Force -Path $out | Out-Null
$hostExe = Join-Path $repo 'native-shell/CoreVideoPro.EngineHost/bin/Release/net9.0/CoreVideoPro.EngineHost.exe'
$ui = Join-Path $repo 'native-shell/CoreVideoPro.WinUI/bin/x64/Release/net9.0-windows10.0.19041.0/win-x64/CoreVideoPro.WinUI.exe'
$profile = 'qa-' + [Guid]::NewGuid().ToString('N')
$native = (Resolve-Path -LiteralPath $NativeCore).Path
$fixture = (Resolve-Path -LiteralPath $Pattern).Path
function Launch([string]$Executable, [string[]]$Arguments, [string]$Log) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = 'Hidden'
    $info.Environment['COREVIDEO_ENGINE_HOST_LOG_DIR'] = (Join-Path $out 'logs')
    foreach ($arg in $Arguments) { $info.ArgumentList.Add($arg) }
    if ($Log) { $info.RedirectStandardError = $true }
    $process = [Diagnostics.Process]::Start($info)
    if ($Log) { $script:receiverLog = $process.StandardError.ReadToEndAsync() }
    return $process
}
function Config([string]$Name, [object[]]$Commands, [int]$Seconds, [bool]$Takeover, [bool]$Shutdown) {
    $path = Join-Path $out "$Name.json"
    @{host=$hostExe;core=$native;profile=$profile;report=(Join-Path $out "$Name-report.json");
      commands=$Commands;seconds=$Seconds;takeControl=$Takeover;shutdown=$Shutdown} |
      ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $path -Encoding utf8
    return $path
}
function WaitReport([string]$Name, [Diagnostics.Process]$Process) {
    $path = Join-Path $out "$Name-report.json"
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    while (!(Test-Path -LiteralPath $path)) {
        if ($Process.HasExited) { throw "$Name exited before a snapshot; see owned logs" }
        if ([DateTime]::UtcNow -gt $deadline) { throw "$Name snapshot timeout" }
        Start-Sleep -Milliseconds 200
    }
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}
$receiver = $null; $first = $null; $second = $null; $stopper = $null
try {
    $receiver = Launch (Get-Command ffmpeg).Source @('-y','-hide_banner','-listen','1','-i',"rtmp://127.0.0.1:$Port/live/probe",'-c','copy',(Join-Path $out 'receiver.flv')) (Join-Path $out 'receiver.log')
    $commands = @(
        @{type='set-output-profile';width=1920;height=1080;fps=60;targetBitrateMbps=8},
        @{type='load-scene-graph';sceneId='host-crash-proof';routes=@(@{routeId='program';mode='fixed';audioRole='mix';mediaAssetId='flash-beep';mediaAssetName='QA flash/beep';mediaAssetKind='video';mediaAssetPath=$fixture;mediaAssetLoop=$true;rect=@{x=0;y=0;width=1;height=1}})},
        @{type='sync-participant-audio-mix';limiterEnabled=$true;channels=@(@{participantId='media';inputLevel=0;muted=$false;noiseSuppression=$false;manualGainDb=0;pan=0;solo=$false;pluginInserts=@();insertSettings=@{}})},
        @{type='sync-audio-routing-matrix';sends=@(@{sourceId='media';busId='master';gainDb=0},@{sourceId='media';busId='stream';gainDb=0})},
        @{type='start-program-output';destinations=@('recording','rtmp');isoParticipantIds=@();destinationSettings=@(@{id='rtmp';label='owned-local-receiver';protocol='rtmp';url="rtmp://127.0.0.1:$Port/live";streamKey='probe';fps=60;targetBitrateMbps=8})},
        @{type='set-recording-targets';targetFolder=$out;filenamePrefix='host-crash';format='mp4';quality='high';isoParticipantIds=@()},
        @{type='start-recording-session';sessionId=$profile;targetFolder=$out;filenamePrefix='host-crash';format='mp4';quality='high';isoParticipantIds=@()}
    )
    $firstConfig = Config 'first' @(@{type='media-core-sync';commands=$commands}) 120 $false $false
    $first = Launch $ui @('--verify-engine-host',$firstConfig) ''
    $null = WaitReport 'first' $first
    Start-Sleep -Seconds 8
    $before = Get-Content (Join-Path $out 'first-report.json') -Raw | ConvertFrom-Json
    $before | ConvertTo-Json -Depth 70 | Set-Content (Join-Path $out 'before-kill.json')
    $killAt = [DateTimeOffset]::UtcNow
    $first.Kill() # Deliberately kill ONLY this owned WinUI process, never its host/core.
    $first.WaitForExit()
    Start-Sleep -Seconds 5
    $secondConfig = Config 'second' @() 5 $false $false
    $second = Launch $ui @('--verify-engine-host',$secondConfig) ''
    $null = WaitReport 'second' $second
    if (!$second.WaitForExit(15000)) { throw 'Replacement console did not finish' }
    $after = Get-Content (Join-Path $out 'second-report.json') -Raw | ConvertFrom-Json
    if ($before.corePid -ne $after.corePid) { throw 'Native core restarted' }
    if (!$after.readOnly) { throw 'Replacement console was not observation-only' }
    @{scope='compiled WinUI bridge probe; generated media; local recording/RTMP';killAt=$killAt;
      before=$before;after=$after;processSurvived=$true;framePerformancePassed=$false;
      note='Receiver/file analysis required; process survival alone is not continuity acceptance.'} |
        ConvertTo-Json -Depth 80 | Set-Content (Join-Path $out 'report.json')
} finally {
    # Explicit takeover and finalization affect only the unique owned QA profile.
    $stopConfig = Config 'stop' @(@{type='media-core-sync';commands=@(@{type='stop-recording-session';reason='Owned crash drill complete'},@{type='stop-program-output'})}) 3 $true $true
    try {
        $stopper = Launch $ui @('--verify-engine-host',$stopConfig) ''
        if (!$stopper.WaitForExit(30000)) { throw 'Owned host shutdown timed out' }
        if ($stopper.ExitCode -ne 0) { Write-Warning 'Owned host shutdown failed; inspect logs and exact QA profile before cleanup.' }
    } finally {
        foreach ($process in @($first,$second,$stopper)) {
            if ($process -and !$process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        }
        if ($receiver) {
            if (!$receiver.WaitForExit(10000)) { $receiver.Kill(); $receiver.WaitForExit(5000) | Out-Null }
            if ($script:receiverLog) { $script:receiverLog.GetAwaiter().GetResult() | Set-Content (Join-Path $out 'receiver.log') }
        }
    }
}
