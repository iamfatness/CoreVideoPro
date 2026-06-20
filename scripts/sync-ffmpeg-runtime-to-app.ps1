# Copy FFmpeg runtime files beside the packaged native core when available.
# Missing FFmpeg is non-fatal: RTMP send-proof validation will report a warning
# and the app can still run recording/native-shell flows.
param(
  [Parameter(Mandatory = $true)]
  [string]$AppDir,
  [string]$FfmpegBinDir = $(if ($env:COREVIDEO_FFMPEG_BIN_DIR) { $env:COREVIDEO_FFMPEG_BIN_DIR } elseif ($env:FFMPEG_BIN_DIR) { $env:FFMPEG_BIN_DIR } else { "" })
)

$ErrorActionPreference = "Stop"

function Write-FfmpegRuntimeManifest {
  param(
    [string]$Status,
    [AllowNull()][string]$SourceDir,
    [string[]]$CopiedDlls = @(),
    [string[]]$MissingPatterns = @(),
    [string]$Warning = ""
  )

  $manifest = @{
    status = $Status
    sourceDir = $(if ($SourceDir) { $SourceDir } else { $null })
    copiedDlls = @($CopiedDlls | Sort-Object -Unique)
    missingPatterns = @($MissingPatterns | Sort-Object -Unique)
    warning = $Warning
    stagedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
  } | ConvertTo-Json -Depth 4

  $manifestPath = Join-Path $AppDir "corevideo-ffmpeg-runtime.json"
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $resolvedManifestPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($manifestPath)
  [System.IO.File]::WriteAllText($resolvedManifestPath, $manifest, $utf8NoBom)
}

function Resolve-FfmpegBinDir {
  param([string]$Override)

  if ($Override -and (Test-Path (Join-Path $Override "ffmpeg.exe"))) {
    return $Override
  }

  $wingetRoot = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
  if (Test-Path $wingetRoot) {
    $candidate = Get-ChildItem -Path $wingetRoot -Recurse -File -Filter "ffmpeg.exe" -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 1
    if ($candidate) {
      return $candidate.DirectoryName
    }
  }

  $ffmpeg = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
  if ($ffmpeg) {
    $candidate = Split-Path -Parent $ffmpeg.Source
    if (Test-Path (Join-Path $candidate "ffmpeg.exe")) {
      return $candidate
    }
  }

  return $null
}

if (-not (Test-Path $AppDir)) {
  throw "AppDir not found: $AppDir"
}

$sourceDir = Resolve-FfmpegBinDir -Override $FfmpegBinDir
if (-not $sourceDir) {
  $warning = "FFmpeg runtime not found. Set COREVIDEO_FFMPEG_BIN_DIR to a bin folder containing ffmpeg.exe for RTMP runtime packaging."
  Write-FfmpegRuntimeManifest -Status "missing" -SourceDir $null -CopiedDlls @() -MissingPatterns @("ffmpeg.exe") -Warning $warning
  Write-Warning "[ffmpeg-runtime] $warning"
  return
}

$patterns = @(
  "ffmpeg.exe",
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
  $warning = "No FFmpeg DLLs copied from $sourceDir."
  Write-FfmpegRuntimeManifest -Status "empty" -SourceDir $sourceDir -CopiedDlls @() -MissingPatterns $patterns -Warning $warning
  Write-Warning "[ffmpeg-runtime] $warning"
  return
}

Write-FfmpegRuntimeManifest -Status "staged" -SourceDir $sourceDir -CopiedDlls $copied
Write-Host "[ffmpeg-runtime] staged $($copied.Count) FFmpeg runtime file(s) from $sourceDir" -ForegroundColor DarkGray
