# Package CoreVideo Pro WinUI native shell as an unsigned MSIX demo (single-project dual mode).
# Stages corevideo-native.exe from COREVIDEO_NATIVE_BUILD_DIR when set, else prefers
# native/build-dev/Release (dev adapters) before native/build* CI stub outputs.
param(
  [string]$NativeBuildDir = $(if ($env:COREVIDEO_NATIVE_BUILD_DIR) { $env:COREVIDEO_NATIVE_BUILD_DIR } else { "" }),
  [switch]$ForceLayoutFallback
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj"
$winUiDir = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI"
$assetsDir = Join-Path $winUiDir "Assets"
$payloadDir = Join-Path $winUiDir "msix-payload"
$artifactsDir = Join-Path $repoRoot "artifacts/native"
$outMsix = Join-Path $artifactsDir "CoreVideoPro.msix"
$layoutDir = Join-Path $artifactsDir "msix-layout"
$productExe = "CoreVideo Pro.exe"

function Ensure-MsixAssets {
  $sourceIcon = Join-Path $assetsDir "SourceIcon.png"
  if (-not (Test-Path $sourceIcon)) {
    Write-Host "[pack:native:msix] SourceIcon.png not found; generating simple native placeholder." -ForegroundColor DarkGray
    New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null
    Add-Type -AssemblyName System.Drawing
    $placeholder = New-Object System.Drawing.Bitmap 512, 512
    try {
      $graphics = [System.Drawing.Graphics]::FromImage($placeholder)
      try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(7, 18, 24))
        $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(44, 226, 206))
        try {
          $graphics.FillRectangle($brush, 128, 128, 256, 256)
        } finally {
          $brush.Dispose()
        }
      } finally {
        $graphics.Dispose()
      }
      $placeholder.Save($sourceIcon, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
      $placeholder.Dispose()
    }
  }

  New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null
  Add-Type -AssemblyName System.Drawing

  $sizes = @{
    "StoreLogo.png"           = @(50, 50)
    "Square44x44Logo.png"     = @(44, 44)
    "Square150x150Logo.png"   = @(150, 150)
    "Wide310x150Logo.png"     = @(310, 150)
    "SplashScreen.png"        = @(620, 300)
  }

  $src = [System.Drawing.Image]::FromFile($sourceIcon)
  try {
    foreach ($entry in $sizes.GetEnumerator()) {
      $target = Join-Path $assetsDir $entry.Key
      $width = $entry.Value[0]
      $height = $entry.Value[1]
      $bmp = New-Object System.Drawing.Bitmap $width, $height
      try {
        $graphics = [System.Drawing.Graphics]::FromImage($bmp)
        try {
          $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
          $graphics.DrawImage($src, 0, 0, $width, $height)
        } finally {
          $graphics.Dispose()
        }
        $bmp.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
      } finally {
        $bmp.Dispose()
      }
    }
  } finally {
    $src.Dispose()
  }
}

function Test-NativeCorePresent {
  param([string]$Dir)
  return [bool](Test-Path (Join-Path $Dir "corevideo-native.exe"))
}

function Resolve-NativeSourceDir {
  param([string]$Override)
  if ($Override -and (Test-NativeCorePresent $Override)) {
    return $Override
  }

  # Prefer dev-machine output (build-native-dev.ps1) before CI stub artifacts.
  # CMake writes Release binaries to native/build-dev/; Release/ is a staged copy.
  $candidates = @(
    (Join-Path $repoRoot "native/build-dev"),
    (Join-Path $repoRoot "native/build-dev/Release"),
    (Join-Path $repoRoot "native/build"),
    (Join-Path $repoRoot "native/build/Release")
  )
  foreach ($candidate in $candidates) {
    if (Test-NativeCorePresent $candidate) {
      return $candidate
    }
  }
  return $null
}

function Stage-MsixPayload {
  if (Test-Path $payloadDir) {
    Remove-Item $payloadDir -Recurse -Force
  }
  New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null

  $nativeSource = Resolve-NativeSourceDir -Override $NativeBuildDir
  $stagedNative = $false
  if ($nativeSource) {
    $nativeArtifacts = Get-ChildItem -Path $nativeSource -Filter "corevideo-*" |
      Where-Object {
        -not $_.PSIsContainer -and
        $_.Extension -in ".exe", ".dll" -and
        $_.BaseName -notmatch "-tests$"
      }
    foreach ($artifact in $nativeArtifacts) {
      Copy-Item -Path $artifact.FullName -Destination (Join-Path $payloadDir $artifact.Name) -Force
      Write-Host "[pack:native:msix] staged native binary: $($artifact.Name)" -ForegroundColor DarkGray
      $stagedNative = $true
    }
  } else {
    Write-Warning "[pack:native:msix] native build dir not found; packaging stub-only media core."
  }

  if (-not (Test-Path (Join-Path $payloadDir "corevideo-native.exe"))) {
    throw "corevideo-native.exe was not staged. Run npm run test:native-media-core or set COREVIDEO_NATIVE_BUILD_DIR to a native build output."
  }

  return $stagedNative
}

function Stage-ZoomRuntimePayload {
  Write-Host "[pack:native:msix] syncing Zoom SDK runtime into MSIX payload..." -ForegroundColor Cyan
  & (Join-Path $repoRoot "scripts\sync-zoom-runtime-to-app.ps1") -AppDir $payloadDir
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }

  # Zoom SDK drops a partial Microsoft.UI.Xaml tree that shadows WinUI theme resources
  # when it lands at the app root.
  $shadowedWinUi = Join-Path $payloadDir "Microsoft.UI.Xaml"
  if (Test-Path $shadowedWinUi) {
    Remove-Item $shadowedWinUi -Recurse -Force
    Write-Host "[pack:native:msix] removed shadowing Microsoft.UI.Xaml folder from payload root" -ForegroundColor DarkGray
  }
}

function Stage-FfmpegRuntimePayload {
  Write-Host "[pack:native:msix] syncing optional FFmpeg runtime for RTMP..." -ForegroundColor Cyan
  & (Join-Path $repoRoot "scripts\sync-ffmpeg-runtime-to-app.ps1") -AppDir $payloadDir
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}

function Assert-MsixPayloadReady {
  param([bool]$StagedNative)

  if ($StagedNative -and -not (Test-Path (Join-Path $payloadDir "corevideo-native.exe"))) {
    throw "MSIX payload expected corevideo-native.exe from native build, but it was not staged."
  }

  $runtimeSdkDll = Join-Path $payloadDir "zoom-runtime\windows\x64\bin\sdk.dll"
  if (-not (Test-Path $runtimeSdkDll)) {
    throw "MSIX payload is missing Zoom runtime sdk.dll at $runtimeSdkDll."
  }
}

function Copy-MsixPayloadToLayout {
  param([string]$Destination)

  if (-not (Test-Path $payloadDir)) {
    return
  }

  Copy-Item -Path (Join-Path $payloadDir "*") -Destination $Destination -Recurse -Force
}

function Find-MakeAppx {
  $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
  if (-not (Test-Path $kits)) {
    return $null
  }
  $latest = Get-ChildItem -Path $kits -Directory -Filter "10.*" |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
  if (-not $latest) {
    return $null
  }
  $tool = Join-Path $latest.FullName "x64\makeappx.exe"
  if (Test-Path $tool) { return $tool }
  return $null
}

function Copy-PublishLayout {
  param(
    [string]$PublishDir,
    [string]$Destination
  )
  if (Test-Path $Destination) {
    Remove-Item $Destination -Recurse -Force
  }
  New-Item -ItemType Directory -Path $Destination -Force | Out-Null
  Copy-Item -Path (Join-Path $PublishDir "*") -Destination $Destination -Recurse -Force

  # A developer may have launched the default publish output in place. Never
  # let recordings, logs, crash reports, or support bundles flow into a fallback
  # MSIX layout from that mutable directory.
  foreach ($name in @("Recordings", "Logs", "CrashReports", "SupportBundles")) {
    $runtimeData = Join-Path $Destination $name
    if (Test-Path $runtimeData) {
      Remove-Item $runtimeData -Recurse -Force
      Write-Host "[pack:native:msix] excluded runtime/user data directory: $name" -ForegroundColor Yellow
    }
  }

  $builtExe = Join-Path $Destination "CoreVideoPro.WinUI.exe"
  $renamedExe = Join-Path $Destination $productExe
  if (Test-Path $builtExe) {
    if (Test-Path $renamedExe) {
      Remove-Item $renamedExe -Force
    }
    Rename-Item -Path $builtExe -NewName $productExe
  }
}

function Write-InstallScripts {
  param(
    [string]$ArtifactsRoot,
    [switch]$LayoutOnly
  )

  $installMsix = @"
# Install a signed CoreVideo Pro MSIX.
param(
  [string]`$PackagePath = (Join-Path `$PSScriptRoot "CoreVideoPro.msix")
)

`$ErrorActionPreference = "Stop"
if (-not (Test-Path `$PackagePath)) {
  throw "MSIX not found: `$PackagePath"
}

`$signature = Get-AuthenticodeSignature -LiteralPath `$PackagePath
if (`$signature.Status -eq "NotSigned") {
  throw "The package is unsigned but retains its signing publisher identity. Sign it with scripts/sign-native-msix.ps1 before installing."
}

Write-Host "Installing signed package `$PackagePath..." -ForegroundColor Cyan
Add-AppxPackage -Path `$PackagePath
Write-Host "Installed. Launch 'CoreVideo Pro' from the Start menu." -ForegroundColor Green
"@

  $installLayout = @"
# A loose layout with the signing publisher identity cannot be registered
# unsigned. Use the unpackaged executable for local smoke tests instead.
param(
  [string]`$LayoutDir = `$PSScriptRoot
)

throw "This layout retains its signing publisher identity and cannot be registered unsigned. Launch 'CoreVideo Pro.exe' directly for a local smoke test."
"@

  if (-not $LayoutOnly) {
    Set-Content -Path (Join-Path $ArtifactsRoot "install-msix.ps1") -Value $installMsix -Encoding UTF8
  }
  Set-Content -Path (Join-Path $ArtifactsRoot "install-msix-layout.ps1") -Value $installLayout -Encoding UTF8
}

function Publish-MsixLayoutFallback {
  param(
    [string]$PublishDir,
    [bool]$StagedNative
  )

  Write-Warning "[pack:native:msix] MSIX pack/sign failed; producing unsigned layout fallback."

  Copy-PublishLayout -PublishDir $PublishDir -Destination $layoutDir
  Copy-MsixPayloadToLayout -Destination $layoutDir

  $manifestSource = Join-Path $winUiDir "Package.appxmanifest"
  $manifestTarget = Join-Path $layoutDir "AppxManifest.xml"
  $manifestXml = Get-Content -Path $manifestSource -Raw
  $manifestXml = $manifestXml.Replace('$targetnametoken$.exe', $productExe)
  $manifestXml = $manifestXml.Replace('$targetentrypoint$', 'CoreVideoPro.WinUI.App')
  Set-Content -Path $manifestTarget -Value $manifestXml -Encoding UTF8

  $assetsTarget = Join-Path $layoutDir "Assets"
  New-Item -ItemType Directory -Path $assetsTarget -Force | Out-Null
  Copy-Item -Path (Join-Path $assetsDir "*") -Destination $assetsTarget -Force

  $makeAppx = Find-MakeAppx
  if ($makeAppx) {
    if (Test-Path $outMsix) {
      Remove-Item $outMsix -Force
    }
    & $makeAppx pack /d $layoutDir /p $outMsix /o | Write-Host
    if ($LASTEXITCODE -eq 0 -and (Test-Path $outMsix)) {
      Write-InstallScripts -ArtifactsRoot $artifactsDir
      return "msix"
    }
    Write-Warning "[pack:native:msix] makeappx pack failed; leaving layout only."
  } else {
    Write-Warning "[pack:native:msix] makeappx.exe not found; leaving layout only."
  }

  Write-InstallScripts -ArtifactsRoot $artifactsDir -LayoutOnly
  return "layout"
}

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
  throw "dotnet SDK is required on PATH (.NET 9)."
}

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  throw "node is required on PATH for packaging helpers."
}

# D1: package.json is the single version source of truth; re-stamp the
# appxmanifest + csproj before anything is built or staged (the MSIX Identity
# Version comes straight from Package.appxmanifest).
Write-Host "[pack:native:msix] stamping version from package.json..." -ForegroundColor Cyan
node (Join-Path $repoRoot "scripts\stamp-version.mjs")
if ($LASTEXITCODE -ne 0) {
  throw "Version stamp failed (scripts/stamp-version.mjs exited $LASTEXITCODE)."
}

New-Item -ItemType Directory -Path $artifactsDir -Force | Out-Null

Write-Host "[pack:native:msix] preparing MSIX assets..." -ForegroundColor Cyan
Ensure-MsixAssets

$stagedNative = Stage-MsixPayload
Stage-ZoomRuntimePayload
Stage-FfmpegRuntimePayload
Assert-MsixPayloadReady -StagedNative $stagedNative

$publishArgs = @(
  "publish", $project,
  "-c", "Release",
  "-r", "win-x64",
  "--self-contained", "false",
  "-p:WindowsPackageType=MSIX",
  "-p:AppxPackageSigningEnabled=false",
  "-p:GenerateAppxPackageOnBuild=true",
  "-p:MsixPayloadDir=$payloadDir\"
)

Write-Host "[pack:native:msix] publishing WinUI shell (Release, win-x64, MSIX)..." -ForegroundColor Cyan
$publishSucceeded = $true
if ($ForceLayoutFallback) {
  $publishSucceeded = $false
} else {
  dotnet @publishArgs
  if ($LASTEXITCODE -ne 0) {
    $publishSucceeded = $false
  }
}

$publishDir = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin/Release/net9.0-windows10.0.19041.0/win-x64/publish"
$resultKind = $null

if ($publishSucceeded) {
  $msixCandidates = Get-ChildItem -Path (Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin") -Recurse -Include "*.msix", "*.msixbundle" |
    Sort-Object LastWriteTime -Descending
  $builtMsix = $msixCandidates | Select-Object -First 1

  if ($builtMsix) {
    if (Test-Path $outMsix) {
      Remove-Item $outMsix -Force
    }
    Copy-Item -Path $builtMsix.FullName -Destination $outMsix -Force
    Write-InstallScripts -ArtifactsRoot $artifactsDir
    $resultKind = "msix"
  } else {
    if (-not (Test-Path $publishDir)) {
      dotnet publish $project -c Release -r win-x64 --self-contained false -p:MsixPayloadDir="$payloadDir\"
      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $resultKind = Publish-MsixLayoutFallback -PublishDir $publishDir -StagedNative $stagedNative
  }
} else {
  Write-Warning "[pack:native:msix] dotnet MSIX publish failed; trying unpackaged publish + layout fallback."
  dotnet publish $project -c Release -r win-x64 --self-contained false -p:MsixPayloadDir="$payloadDir\"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  if (-not (Test-Path $publishDir)) {
    throw "Could not find publish output after dotnet publish."
  }
  $resultKind = Publish-MsixLayoutFallback -PublishDir $publishDir -StagedNative $stagedNative
}

Write-Host ""
Write-Host "Native shell MSIX package ready:" -ForegroundColor Green
if ($resultKind -eq "msix") {
  Write-Host "  $outMsix"
  Write-Host "  $(Join-Path $artifactsDir "install-msix.ps1")"
} else {
  Write-Host "  $layoutDir"
  Write-Host "  $(Join-Path $artifactsDir "install-msix-layout.ps1")"
}
if ($stagedNative) {
  Write-Host "  Media core: corevideo-native.exe (+ siblings)" -ForegroundColor Green
} else {
  Write-Host "  Media core: missing" -ForegroundColor Red
}
Write-Host ""
Write-Host "Signing: unsigned release candidate retaining the manifest publisher identity." -ForegroundColor Yellow
Write-Host "  Sign before install: powershell -File scripts/sign-native-msix.ps1" -ForegroundColor DarkGray
Write-Host "  Local smoke without trust setup: launch artifacts/native/msix-layout/'CoreVideo Pro.exe' directly." -ForegroundColor DarkGray
