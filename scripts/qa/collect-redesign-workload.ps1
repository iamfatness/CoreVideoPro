param(
  [ValidateRange(30,7200)][int]$Seconds=1800,
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][int]$AppProcessId,
  [string[]]$AdditionalProcessName=@()
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'workload-samples.ndjson')) {
  throw 'Use a fresh output directory; do not mix workload runs.'
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RedesignWindowObservation {
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
}
'@
$rows=[Collections.Generic.List[object]]::new()
$errors=[Collections.Generic.List[string]]::new()
$initial=(Invoke-RestMethod http://127.0.0.1:8011/snapshot -TimeoutSec 5).snapshot
if (-not $initial.recording.active -or $initial.recording.lifecycle.state -ne 'producing' -or -not $initial.virtualCamera.enabled) {throw 'Required outputs are not producing; do not start the measured interval.'}
$app=Get-Process -Id $AppProcessId
if ($app.ProcessName -ne 'CoreVideoPro.WinUI') {throw 'Select the tested CoreVideo Pro app process'}
$generation="$($app.Id):$($app.StartTime.ToUniversalTime().ToString('o'))"
$inputs=@($initial.realtimeEvidence.monitorWorker.inputs.sources | ForEach-Object sourceId)
$iso=@($initial.recording.streams | Where-Object kind -eq iso | ForEach-Object sourceId)
$started=[DateTime]::UtcNow.ToString('o')
$clock=[Diagnostics.Stopwatch]::StartNew()
do {
  try {
    $envelope=Invoke-RestMethod http://127.0.0.1:8011/snapshot -TimeoutSec 5
    if (-not $envelope.available -or $envelope.ageMs -gt 1000) {throw 'Snapshot unavailable or stale'}
    $s=$envelope.snapshot
    $app.Refresh()
    $window=$app.MainWindowHandle
    $display=[ordered]@{visible=($window -ne 0 -and [RedesignWindowObservation]::IsWindowVisible($window));minimized=[RedesignWindowObservation]::IsIconic($window)}
    $additional=@(foreach($name in $AdditionalProcessName) {
      Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{name=$_.ProcessName;processGeneration="$($_.Id):$($_.StartTime.ToUniversalTime().ToString('o'))";cpuSeconds=$_.CPU}
      }
    })
    $row=[ordered]@{collectedAtMs=$clock.Elapsed.TotalMilliseconds;processGeneration=$generation;displayState=$display;additionalProcesses=$additional;snapshot=[ordered]@{
      outputProfile=$s.outputProfile;meetingState=$s.meetingState;programBuffer=$s.programBuffer
      realtimeEvidence=$s.realtimeEvidence;encoderEvidence=$s.encoderEvidence;virtualCamera=$s.virtualCamera
      recording=[ordered]@{active=$s.recording.active;lifecycle=$s.recording.lifecycle;proof=$s.recording.proof;artifactPath=$s.recording.artifactPath
        streams=@($s.recording.streams | Select-Object kind,sourceId,status,framesWritten,droppedFrames,readiness)}
    }}
    $rows.Add($row)
    ($row | ConvertTo-Json -Depth 24 -Compress) | Add-Content -LiteralPath (Join-Path $OutputDirectory 'workload-samples.ndjson')
    if (-not $s.recording.active -or -not $s.virtualCamera.enabled) {$errors.Add('A required output stopped during measurement.');break}
    if (-not (Get-Process -Id $app.Id -ErrorAction SilentlyContinue)) {$errors.Add('Audited app exited.');break}
  } catch {$errors.Add($_.Exception.Message);break}
  if ($clock.Elapsed.TotalSeconds -ge $Seconds) {break}
  Start-Sleep -Seconds 5
} while ($true)
$report=[ordered]@{startTime=$started;endTime=[DateTime]::UtcNow.ToString('o');appPath=$app.Path;requiredInputIds=$inputs;requiredIsoIds=$iso;requiredAdditionalProcesses=$AdditionalProcessName;samples=$rows
  errors=$errors;monitorProbeVerdict=$null;cameraPixelVerdict=$null
  framePerformanceReport=@{startTime=$started;endTime=[DateTime]::UtcNow.ToString('o');soakPassed=$false;errorMatches=$errors
    performanceEvidence=@{metricVersion='anchored-deadline-v1';targetFps=60;clock='monotonic'
      requiredPaths=@('cpu-submission','program-gpu','program-presentation','preview-presentation','multiview-presentation','recording:program','virtual-camera:program')+@($iso | ForEach-Object {'recording:iso:'+$_})
      enabledOutputs=@('recording:program','virtual-camera:program')+@($iso | ForEach-Object {'recording:iso:'+$_});paths=@()}}
}
$report | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'workload-report.json')
