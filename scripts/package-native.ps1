# Package CoreVideo Pro WinUI native shell for demo distribution (unpackaged folder layout).
# Stages corevideo-native.exe from COREVIDEO_NATIVE_BUILD_DIR when set, else prefers
# native/build-dev/ (dev adapters) before native/build* CI stub outputs.
param(
  [string]$NativeBuildDir = $(if ($env:COREVIDEO_NATIVE_BUILD_DIR) { $env:COREVIDEO_NATIVE_BUILD_DIR } else { "" })
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj"
$outDir = Join-Path $repoRoot "artifacts\native\win-unpacked"
$productExe = "CoreVideoPro.WinUI.exe"

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
  throw "dotnet SDK is required on PATH (.NET 9)."
}

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  throw "node is required on PATH for packaging helpers."
}

Write-Host "[pack:native] publishing WinUI shell (Release, win-x64)..." -ForegroundColor Cyan
dotnet publish $project -c Release -r win-x64 --self-contained false
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

$publishDir = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish"
if (-not (Test-Path (Join-Path $publishDir "CoreVideoPro.WinUI.dll"))) {
  $publishDll = Get-ChildItem -Path (Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin") -Recurse -Filter "CoreVideoPro.WinUI.dll" |
    Where-Object { $_.DirectoryName -match "\\publish$" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if (-not $publishDll) {
    throw "Could not find CoreVideoPro.WinUI.dll after dotnet publish."
  }
  $publishDir = $publishDll.DirectoryName
}

Write-Host "[pack:native] staging $outDir ..." -ForegroundColor Cyan
if (Test-Path $outDir) {
  Remove-Item $outDir -Recurse -Force
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Copy-Item -Path (Join-Path $publishDir "*") -Destination $outDir -Recurse -Force

# Older publishes could leave a nested publish\ tree; it breaks WinUI resource lookup.
$nestedPublish = Join-Path $outDir "publish"
if (Test-Path $nestedPublish) {
  Remove-Item $nestedPublish -Recurse -Force
}

$xbfCount = (Get-ChildItem -Path $outDir -Filter "*.xbf" -Recurse -File -ErrorAction SilentlyContinue).Count
if ($xbfCount -eq 0) {
  Write-Warning "[pack:native] no .xbf files staged; WinUI shell may fail to launch."
} else {
  Write-Host "[pack:native] staged $xbfCount XAML .xbf file(s)" -ForegroundColor DarkGray
}

$launcherBat = Join-Path $outDir "CoreVideo Pro.bat"
@(
  "@echo off",
  "setlocal",
  "cd /d ""%~dp0""",
  "start """" ""%~dp0CoreVideoPro.WinUI.exe"" %*"
) | Set-Content -Path $launcherBat -Encoding ASCII

function Test-NativeCorePresent {
  param([string]$Dir)
  return [bool](Test-Path (Join-Path $Dir "corevideo-native.exe"))
}

function Get-NativeSourceLabel {
  param([string]$Dir)
  $devRelease = Join-Path $repoRoot "native\build-dev\Release"
  $devRoot = Join-Path $repoRoot "native\build-dev"
  $stubRelease = Join-Path $repoRoot "native\build\Release"
  $stubRoot = Join-Path $repoRoot "native\build"

  if ($Dir -ieq $devRelease) {
    return "dev adapters (build-dev/Release)"
  }
  if ($Dir -ieq $devRoot) {
    return "dev build (build-dev)"
  }
  if ($Dir -ieq $stubRelease) {
    return "CI stub (build/Release)"
  }
  if ($Dir -ieq $stubRoot) {
    return "CI stub (build)"
  }
  return "custom ($Dir)"
}

function Resolve-NativeSourceDir {
  param([string]$Override)
  if ($Override -and (Test-NativeCorePresent $Override)) {
    return $Override
  }

  # Prefer the dev-machine output from scripts/build-native-dev.ps1
  # (COREVIDEO_ENABLE_DEV_ADAPTERS=ON). CMake writes Release binaries to
  # native/build-dev/ (not build-dev/Release); the Release subfolder is a staged copy.
  $candidates = @(
    (Join-Path $repoRoot "native\build-dev"),
    (Join-Path $repoRoot "native\build-dev\Release"),
    (Join-Path $repoRoot "native\build"),
    (Join-Path $repoRoot "native\build\Release")
  )
  foreach ($candidate in $candidates) {
    if (Test-NativeCorePresent $candidate) {
      return $candidate
    }
  }
  return $null
}

$nativeSource = Resolve-NativeSourceDir -Override $NativeBuildDir
$stagedNative = $false
if ($nativeSource) {
  Write-Host "[pack:native] using native core from $(Get-NativeSourceLabel $nativeSource)" -ForegroundColor DarkGray
  $nativeArtifacts = Get-ChildItem -Path $nativeSource -Filter "corevideo-*" |
    Where-Object {
      -not $_.PSIsContainer -and
      $_.Extension -in ".exe", ".dll" -and
      $_.BaseName -notmatch "-tests$"
    }
  foreach ($artifact in $nativeArtifacts) {
    Copy-Item -Path $artifact.FullName -Destination (Join-Path $outDir $artifact.Name) -Force
    Write-Host "[pack:native] staged native binary: $($artifact.Name)" -ForegroundColor DarkGray
    $stagedNative = $true
  }
} else {
  Write-Warning "[pack:native] native build dir not found; packaging stub-only media core."
}

if (-not (Test-Path (Join-Path $outDir "corevideo-native.exe"))) {
  throw "corevideo-native.exe was not staged. Run npm run test:native-media-core or set COREVIDEO_NATIVE_BUILD_DIR to a native build output."
}

Write-Host "[pack:native] syncing Zoom SDK runtime beside packaged app..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\sync-zoom-runtime-to-app.ps1") -AppDir $outDir
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

Write-Host "[pack:native] syncing optional FFmpeg runtime for RTMP..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\sync-ffmpeg-runtime-to-app.ps1") -AppDir $outDir
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

# Zoom SDK drops a partial Microsoft.UI.Xaml tree that shadows WinUI theme resources.
$shadowedWinUi = Join-Path $outDir "Microsoft.UI.Xaml"
if (Test-Path $shadowedWinUi) {
  Remove-Item $shadowedWinUi -Recurse -Force
  Write-Host "[pack:native] removed shadowing Microsoft.UI.Xaml folder from app root" -ForegroundColor DarkGray
}

$launcherExe = Join-Path $outDir "CoreVideoPro.WinUI.exe"
$desktopDir = [Environment]::GetFolderPath("Desktop")
$desktopShortcut = Join-Path $desktopDir "CoreVideo Pro.lnk"
if ((Test-Path $launcherExe) -and (Test-Path $desktopDir)) {
  $shell = New-Object -ComObject WScript.Shell
  $shortcut = $shell.CreateShortcut($desktopShortcut)
  $shortcut.TargetPath = $launcherExe
  $shortcut.WorkingDirectory = $outDir
  $shortcut.Description = "CoreVideo Pro"
  $shortcut.Save()
  Write-Host "[pack:native] refreshed desktop shortcut: $desktopShortcut" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "Native shell package ready:" -ForegroundColor Green
Write-Host "  $outDir"
Write-Host "  $launcherExe"
Write-Host "  $(Join-Path $outDir "CoreVideo Pro.bat")"
if ($stagedNative) {
  Write-Host "  Media core: corevideo-native.exe (+ siblings)" -ForegroundColor Green
} else {
  Write-Host "  Media core: missing" -ForegroundColor Red
}
