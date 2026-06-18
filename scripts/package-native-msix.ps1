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
  $sourceIcon = Join-Path $repoRoot "desktop/build/icon.png"
  if (-not (Test-Path $sourceIcon)) {
    Write-Host "[pack:native:msix] generating desktop icons..." -ForegroundColor DarkGray
    Push-Location $repoRoot
    try {
      node desktop/scripts/generate-icons.mjs
      if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
      Pop-Location
    }
  }

  if (-not (Test-Path $sourceIcon)) {
    throw "Missing desktop/build/icon.png (run npm run generate:icons)."
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
    $desktopOut = Join-Path $payloadDir "desktop"
    New-Item -ItemType Directory -Path $desktopOut -Force | Out-Null
    Copy-Item -Path (Join-Path $repoRoot "desktop/coreStub.cjs") -Destination (Join-Path $desktopOut "coreStub.cjs") -Force
    Write-Host "[pack:native:msix] staged desktop/coreStub.cjs (stub fallback)" -ForegroundColor DarkGray
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
# Install CoreVideo Pro MSIX demo (unsigned / dev certificate).
param(
  [string]`$PackagePath = (Join-Path `$PSScriptRoot "CoreVideoPro.msix")
)

`$ErrorActionPreference = "Stop"
if (-not (Test-Path `$PackagePath)) {
  throw "MSIX not found: `$PackagePath"
}

Write-Host "Installing `$PackagePath (AllowUnsigned)..." -ForegroundColor Cyan
Add-AppxPackage -Path `$PackagePath -AllowUnsigned
Write-Host "Installed. Launch 'CoreVideo Pro' from the Start menu." -ForegroundColor Green
"@

  $installLayout = @"
# Register CoreVideo Pro MSIX layout (unsigned; for CI when MakeAppx/signing is unavailable).
param(
  [string]`$LayoutDir = `$PSScriptRoot
)

`$ErrorActionPreference = "Stop"
`$manifest = Join-Path `$LayoutDir "AppxManifest.xml"
if (-not (Test-Path `$manifest)) {
  throw "AppxManifest.xml not found in layout: `$LayoutDir"
}

Write-Host "Registering layout from `$LayoutDir (AllowUnsigned)..." -ForegroundColor Cyan
Add-AppxPackage -Register -Path `$manifest -AllowUnsigned
Write-Host "Registered. Launch 'CoreVideo Pro' from the Start menu." -ForegroundColor Green
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
  throw "node is required on PATH (coreStub fallback bundling)."
}

New-Item -ItemType Directory -Path $artifactsDir -Force | Out-Null

Write-Host "[pack:native:msix] preparing MSIX assets..." -ForegroundColor Cyan
Ensure-MsixAssets

Write-Host "[pack:native:msix] bundling coreStub fallback..." -ForegroundColor Cyan
Push-Location $repoRoot
try {
  node --input-type=module -e "import { ensureCoreStubBundle } from './desktop/scripts/desktopRuntime.mjs'; ensureCoreStubBundle();"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
  Pop-Location
}

$stagedNative = Stage-MsixPayload
Stage-ZoomRuntimePayload
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
  Write-Host "  Media core: Node stub (requires Node.js on PATH)" -ForegroundColor Yellow
}
Write-Host ""
Write-Host "Signing: unsigned demo package (AppxPackageSigningEnabled=false)." -ForegroundColor Yellow
Write-Host "  Local install: powershell -File artifacts/native/install-msix.ps1" -ForegroundColor DarkGray
Write-Host "  Or: Add-AppxPackage -Path artifacts/native/CoreVideoPro.msix -AllowUnsigned" -ForegroundColor DarkGray
