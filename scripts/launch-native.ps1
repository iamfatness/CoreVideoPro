$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj"

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw ".NET SDK is required on PATH. Install .NET 9 from https://dotnet.microsoft.com/download/dotnet/9.0"
}

Write-Host "Building CoreVideo Pro (WinUI native shell, framework-dependent + Runtime 2.2)..."
dotnet publish $project -c Release -r win-x64 --self-contained false

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$publishDir = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin/Release/net9.0-windows10.0.19041.0/win-x64/publish"
$exe = Join-Path $publishDir "CoreVideoPro.WinUI.exe"

if (-not (Test-Path $exe)) {
    $found = Get-ChildItem -Path (Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin") -Recurse -Filter "CoreVideoPro.WinUI.exe" |
        Where-Object { $_.DirectoryName -match "\\publish$" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($found) {
        $exe = $found.FullName
        $publishDir = $found.DirectoryName
    }
}

if (-not (Test-Path $exe)) {
    Write-Error "Could not find CoreVideoPro.WinUI.exe after build."
}

function Sync-NativeCoreArtifacts {
    param([string]$TargetDir)
    $nativeCandidates = @(
        (Join-Path $repoRoot "native\build-dev"),
        (Join-Path $repoRoot "native\build-dev\Release"),
        (Join-Path $repoRoot "native\build"),
        (Join-Path $repoRoot "native\build\Release")
    )
    foreach ($candidate in $nativeCandidates) {
        if (-not (Test-Path (Join-Path $candidate "corevideo-native.exe"))) {
            continue
        }
        Write-Host "[launch:native] staging native core from $candidate" -ForegroundColor DarkGray
        Get-ChildItem -Path $candidate -Filter "corevideo-*" -File |
            Where-Object { $_.Extension -in ".exe", ".dll" -and $_.BaseName -notmatch "-tests$" } |
            ForEach-Object {
                Copy-Item -Path $_.FullName -Destination (Join-Path $TargetDir $_.Name) -Force
            }
        return
    }
    Write-Host "[launch:native] native build not found; publish may run stub-only media core" -ForegroundColor Yellow
}

Write-Host "[launch:native] syncing Zoom SDK runtime beside publish output..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\sync-zoom-runtime-to-app.ps1") -AppDir $publishDir
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "[launch:native] syncing FFmpeg runtime beside publish output..." -ForegroundColor Cyan
& (Join-Path $repoRoot "scripts\sync-ffmpeg-runtime-to-app.ps1") -AppDir $publishDir
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$shadowedWinUi = Join-Path $publishDir "Microsoft.UI.Xaml"
if (Test-Path $shadowedWinUi) {
    Remove-Item $shadowedWinUi -Recurse -Force
    Write-Host "[launch:native] removed shadowing Microsoft.UI.Xaml folder" -ForegroundColor DarkGray
}

Sync-NativeCoreArtifacts -TargetDir $publishDir

Write-Host "Launching $exe..."
$launchLog = Join-Path $env:LOCALAPPDATA "CoreVideoPro\launch.log"

function Get-LaunchLogTail {
    if (-not (Test-Path $launchLog)) {
        return $null
    }

    return (Get-Content $launchLog -Tail 8 -ErrorAction SilentlyContinue) -join [Environment]::NewLine
}

function Get-RunningStudioProcess {
    Get-Process -Name "CoreVideoPro.WinUI" -ErrorAction SilentlyContinue |
        Where-Object { -not $_.HasExited } |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
}

$existing = Get-RunningStudioProcess
if ($existing) {
    Write-Host "[launch:native] CoreVideo Pro already running (pid $($existing.Id)). Check the taskbar if the window is hidden." -ForegroundColor Green
    exit 0
}

$process = Start-Process -FilePath $exe -WorkingDirectory $publishDir -PassThru
Start-Sleep -Seconds 8

$running = if (-not $process.HasExited) { $process } else { Get-RunningStudioProcess }

if ($null -eq $running) {
    $tail = Get-LaunchLogTail
    Write-Host "[launch:native] CoreVideo Pro exited before the window stayed open (code $($process.ExitCode))." -ForegroundColor Red
    if ($tail) {
        Write-Host "[launch:native] Recent launch.log:" -ForegroundColor Yellow
        Write-Host $tail
    }
    Write-Host "[launch:native] Full log: $launchLog" -ForegroundColor DarkGray
    exit $(if ($process.ExitCode -ne 0) { $process.ExitCode } else { 1 })
}

if ($process.HasExited -and $running.Id -ne $process.Id) {
    Write-Host "[launch:native] Secondary launch redirected to primary instance (pid $($running.Id))." -ForegroundColor Cyan
}

Write-Host "[launch:native] CoreVideo Pro is running (pid $($running.Id))." -ForegroundColor Green
Start-Process -FilePath "explorer.exe" -ArgumentList $publishDir | Out-Null
