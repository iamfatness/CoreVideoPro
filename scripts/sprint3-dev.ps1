# Sprint 3 dev launcher: native output path (D3D11 + MF MP4 + RTMP proof).
# Prerequisites:
#   1. npm install
#   2. npm install --no-save electron@latest
#   3. .\scripts\build-native-dev.ps1
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\native\build-dev"),
  [string]$MeetingUrl = "",
  [switch]$SkipRendererBuild
)

& (Join-Path $PSScriptRoot "sprint1-dev.ps1") -BuildDir $BuildDir -MeetingUrl $MeetingUrl -SkipRendererBuild:$SkipRendererBuild