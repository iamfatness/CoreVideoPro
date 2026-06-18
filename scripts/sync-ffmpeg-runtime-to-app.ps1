# Copy FFmpeg runtime DLLs beside the packaged native core when available.
# Missing FFmpeg is non-fatal: RTMP send-proof validation will report a warning
# and the app can still run recording/native-shell flows.
param(
  [Parameter(Mandatory = $true)]
  [string]$AppDir,
  [string]$FfmpegBinDir = $(if ($env:COREVIDEO_FFMPEG_BIN_DIR) { $env:COREVIDEO_FFMPEG_BIN_DIR } elseif ($env:FFMPEG_BIN_DIR) { $env:FFMPEG_BIN_DIR } else { "" })
)

$ErrorActionPreference = "Stop"

function Resolve-FfmpegBinDir {
  param([string]$Override)

  if ($Override -and (Test-Path (Join-Path $Override "avformat*.dll"))) {
    return $Override
  }

  $ffmpeg = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
  if ($ffmpeg) {
    $candidate = Split-Path -Parent $ffmpeg.Source
    if (Test-Path (Join-Path $candidate "avformat*.dll")) {
      return $candidate
    }
  }

  $wingetRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
  if (Test-Path $wingetRoot) {
    $candidate = Get-ChildItem -Path $wingetRoot -Recurse -File -Filter "avformat*.dll" -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 1
    if ($candidate) {
      return $candidate.DirectoryName
    }
  }

  return $null
}

if (-not (Test-Path $AppDir)) {
  throw "AppDir not found: $AppDir"
}

$sourceDir = Resolve-FfmpegBinDir -Override $FfmpegBinDir
if (-not $sourceDir) {
  Write-Warning "[ffmpeg-runtime] FFmpeg runtime not found. Set COREVIDEO_FFMPEG_BIN_DIR to a bin folder containing avformat*.dll for RTMP runtime packaging."
  return
}

$patterns = @(
  "avformat*.dll",
  "avcodec*.dll",
  "avutil*.dll",
  "swresample*.dll",
  "swscale*.dll",
  "avdevice*.dll",
  "avfilter*.dll",
  "postproc*.dll"
)

$copied = @()
foreach ($pattern in $patterns) {
  Get-ChildItem -Path $sourceDir -Filter $pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination (Join-Path $AppDir $_.Name) -Force
    $copied += $_.Name
  }
}

if ($copied.Count -eq 0) {
  Write-Warning "[ffmpeg-runtime] No FFmpeg DLLs copied from $sourceDir."
  return
}

$manifest = @{
  sourceDir = $sourceDir
  copiedDlls = $copied | Sort-Object -Unique
  stagedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
} | ConvertTo-Json -Depth 4

Set-Content -Path (Join-Path $AppDir "corevideo-ffmpeg-runtime.json") -Value $manifest -Encoding UTF8
Write-Host "[ffmpeg-runtime] staged $($copied.Count) FFmpeg runtime DLL(s) from $sourceDir" -ForegroundColor DarkGray
