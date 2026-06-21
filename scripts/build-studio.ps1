param(
  [string]$Config = "Debug",
  [string]$NativeDir = (Join-Path $PSScriptRoot "..\native"),
  [string]$NativeBuildDir = (Join-Path $PSScriptRoot "..\native\build"),
  [string]$StudioDir = (Join-Path $PSScriptRoot "..\studio"),
  [string]$StudioBuildDir = (Join-Path $PSScriptRoot "..\studio\build-clean")
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-CMake {
  $cmake = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmake) {
    return $cmake.Source
  }

  $candidate = "${env:ProgramFiles}\CMake\bin\cmake.exe"
  if (Test-Path $candidate) {
    return $candidate
  }

  throw "CMake was not found. Install CMake for Windows and make sure cmake.exe is on PATH, or install Visual Studio with CMake tools."
}

function Resolve-VsDevCmd {
  $candidates = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  )

  $candidate = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($candidate) {
    return $candidate
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $discovered = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "Common7\Tools\VsDevCmd.bat" | Select-Object -First 1
    if ($discovered -and (Test-Path $discovered)) {
      return $discovered
    }
  }

  throw "Visual Studio Developer Command Prompt was not found. Install Visual Studio 18 Community with the Desktop development with C++ workload, or install Build Tools with MSVC and CMake support."
}

function Quote-CmdArg([string]$Value) {
  return '"' + ($Value -replace '"', '\"') + '"'
}

function Invoke-VsDevCommand {
  param(
    [string]$VsDevCmd,
    [string[]]$Commands
  )

  $command = (@(
    "set Path=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem",
    "call $(Quote-CmdArg $VsDevCmd) -arch=amd64"
  ) + $Commands) -join " && "

  cmd.exe /d /c $command
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}

function Invoke-CMakeConfigure {
  param(
    [string]$CMake,
    [string]$SourceDir,
    [string]$BuildDir,
    [string[]]$ExtraArgs
  )

  $args = @("-S", $SourceDir, "-B", $BuildDir) + $ExtraArgs
  return "$(Quote-CmdArg $CMake) $(($args | ForEach-Object { Quote-CmdArg $_ }) -join ' ')"
}

function Invoke-CMakeBuild {
  param(
    [string]$CMake,
    [string]$BuildDir,
    [string]$Config,
    [string[]]$Targets
  )

  $args = @("--build", $BuildDir, "--config", $Config)
  if ($Targets.Count -gt 0) {
    $args += "--target"
    $args += $Targets
  }
  return "$(Quote-CmdArg $CMake) $(($args | ForEach-Object { Quote-CmdArg $_ }) -join ' ')"
}

$cmake = Resolve-CMake
$vsDevCmd = Resolve-VsDevCmd

Write-Host "[build-studio] using Visual Studio environment: $vsDevCmd" -ForegroundColor DarkGray
Write-Host "[build-studio] building native media core..." -ForegroundColor Cyan
Invoke-VsDevCommand -VsDevCmd $vsDevCmd -Commands @(
  (Invoke-CMakeConfigure -CMake $cmake -SourceDir $NativeDir -BuildDir $NativeBuildDir -ExtraArgs @(
      "-DCOREVIDEO_STUB=ON",
      "-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON",
      "-DCOREVIDEO_WITH_WASAPI_MONITOR=ON",
      "-DCOREVIDEO_WITH_WASAPI_CAPTURE=ON",
      "-DBUILD_TESTING=OFF"
    )),
  (Invoke-CMakeBuild -CMake $cmake -BuildDir $NativeBuildDir -Config $Config -Targets @("corevideo-native"))
)

$nativeExe = Join-Path $repoRoot "native\build\corevideo-native.exe"
if (-not (Test-Path $nativeExe)) {
  throw "Expected native media core at $nativeExe after build."
}

Write-Host "[build-studio] building native Studio shell..." -ForegroundColor Cyan
Invoke-VsDevCommand -VsDevCmd $vsDevCmd -Commands @(
  (Invoke-CMakeConfigure -CMake $cmake -SourceDir $StudioDir -BuildDir $StudioBuildDir -ExtraArgs @()),
  (Invoke-CMakeBuild -CMake $cmake -BuildDir $StudioBuildDir -Config $Config -Targets @("CoreVideoStudio"))
)

$studioExe = Join-Path $repoRoot "studio\build-clean\$Config\CoreVideoStudio.exe"
if (-not (Test-Path $studioExe)) {
  throw "Expected native Studio shell at $studioExe after build."
}

Write-Host ""
Write-Host "[build-studio] built native Studio stack." -ForegroundColor Green
Write-Host "  Media core: $nativeExe"
Write-Host "  Studio:     $studioExe"
