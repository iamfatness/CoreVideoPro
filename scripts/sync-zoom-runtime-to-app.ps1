# Copy staged Zoom Meeting SDK runtime files beside the WinUI app for packaged launches.
param(
  [Parameter(Mandatory = $true)]
  [string]$AppDir,
  [string]$RuntimeDir = $(if ($env:COREVIDEO_ZOOM_RUNTIME_DIR) { $env:COREVIDEO_ZOOM_RUNTIME_DIR } else { "" })
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = [System.IO.Path]::GetFullPath($AppDir)

if (-not (Test-Path $appDir)) {
  throw "App directory not found: $appDir"
}

if ([string]::IsNullOrWhiteSpace($RuntimeDir)) {
  $RuntimeDir = Join-Path $repoRoot "native-core\zoom-runtime\windows\x64"
}

$runtimeDir = [System.IO.Path]::GetFullPath($RuntimeDir)
$binSource = Join-Path $runtimeDir "bin"
if (-not (Test-Path (Join-Path $binSource "sdk.dll"))) {
  Write-Host "[sync:zoom] staged runtime missing at $runtimeDir; running stage-zoom-sdk.ps1" -ForegroundColor Yellow
  & (Join-Path $PSScriptRoot "stage-zoom-sdk.ps1")
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
  $runtimeDir = Join-Path $repoRoot "native-core\zoom-runtime\windows\x64"
  $binSource = Join-Path $runtimeDir "bin"
}

if (-not (Test-Path (Join-Path $binSource "sdk.dll"))) {
  throw "Zoom SDK runtime is still missing after staging. Set ZOOM_SDK_DIR and run .\scripts\stage-zoom-sdk.ps1."
}

$packagedRuntime = Join-Path $appDir "zoom-runtime\windows\x64"
$packagedBin = Join-Path $packagedRuntime "bin"
$packagedLib = Join-Path $packagedRuntime "lib"
$packagedHeaders = Join-Path $packagedRuntime "h"

$reservedNames = @(
  "Microsoft.UI.Xaml",
  "Microsoft.WinUI.dll",
  "CoreVideoPro.WinUI.exe",
  "CoreVideo Pro.exe"
)

Write-Host "[sync:zoom] copying runtime DLLs to $appDir" -ForegroundColor Cyan
Get-ChildItem -Path $binSource -Force | ForEach-Object {
  $item = $_
  if ($reservedNames -contains $item.Name) {
    Write-Host "[sync:zoom] skip reserved app artifact: $($item.Name)" -ForegroundColor DarkGray
    return
  }
  try {
    Copy-Item -Path $item.FullName -Destination $appDir -Recurse -Force -ErrorAction Stop
  } catch [System.IO.IOException] {
    # A hung/zombie zoom-engine can hold runtime DLLs locked. If the staged
    # copy is same-size the lock is harmless - warn and continue rather than
    # failing the whole launch (SDK runtime files change ~never). NOTE: $_ in
    # catch is the EXCEPTION - use the captured $item.
    $existing = Join-Path $appDir $item.Name
    if ((Test-Path $existing) -and -not $item.PSIsContainer -and
        ((Get-Item $existing).Length -eq $item.Length)) {
      Write-Host "[sync:zoom] locked but same-size, keeping existing: $($item.Name)" -ForegroundColor Yellow
    } else {
      throw
    }
  }
}

Write-Host "[sync:zoom] packaging layout at $packagedRuntime" -ForegroundColor Cyan
if (Test-Path $packagedRuntime) {
  Remove-Item $packagedRuntime -Recurse -Force
}
New-Item -ItemType Directory -Path $packagedBin -Force | Out-Null
New-Item -ItemType Directory -Path $packagedLib -Force | Out-Null
New-Item -ItemType Directory -Path $packagedHeaders -Force | Out-Null

Copy-Item -Path (Join-Path $binSource "*") -Destination $packagedBin -Recurse -Force
Copy-Item -Path (Join-Path $runtimeDir "lib\sdk.lib") -Destination (Join-Path $packagedLib "sdk.lib") -Force
Copy-Item -Path (Join-Path $runtimeDir "h\*") -Destination $packagedHeaders -Recurse -Force

Write-Host "[sync:zoom] Zoom runtime synced to app folder" -ForegroundColor Green