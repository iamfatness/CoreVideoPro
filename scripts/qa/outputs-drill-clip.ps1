<#
.SYNOPSIS
  Makes the synthetic motion clip the outputs drill routes into Program with
  -NoWebcam (docs/qa/outputs-drill.md): 1920x1080, exactly 60 fps, H.264, long
  enough to outlast the run, since a media-bin clip plays once and never loops.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\qa\outputs-drill-clip.ps1 -Seconds 2400 -Out D:\CoreVideo-QA\media\drill-motion-2400s.mp4
#>
param(
  [int]$Seconds = 2400,
  [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'
if ($Seconds -lt 60) { throw 'Seconds must be at least 60.' }
$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$null = New-Item -ItemType Directory -Path (Split-Path -Parent $Out) -Force
# testsrc: moving bars, a running counter and a sweep; it compresses to a few
# Mbit/s at 1080p60 (testsrc2 is 10x larger). -g 120 keeps seeks cheap.
& $ffmpeg -v error -y -f lavfi -i "testsrc=size=1920x1080:rate=60" -t $Seconds -c:v libx264 -preset veryfast -crf 26 -pix_fmt yuv420p -g 120 -movflags +faststart -an $Out
if ($LASTEXITCODE -ne 0) { throw "ffmpeg exited $LASTEXITCODE" }
. (Join-Path $PSScriptRoot 'outputs-drill-lib.ps1')
$problems = @(Test-DrillMediaClip -ClipPath $Out -Ffprobe (Get-Command ffprobe -ErrorAction Stop).Source -MinimumSeconds $Seconds)
if ($problems.Count) { throw ("The clip does not meet the drill's rule: " + ($problems -join '; ')) }
"Clip ready: $Out ($Seconds s, 1920x1080 @ 60 fps, H.264); use it with outputs-drill.ps1 -NoWebcam -MediaClip"
