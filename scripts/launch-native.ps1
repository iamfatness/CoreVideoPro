$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj"

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw ".NET SDK is required on PATH. Install .NET 9 from https://dotnet.microsoft.com/download/dotnet/9.0"
}

Write-Host "Building CoreVideo Pro (WinUI native shell)..."
dotnet publish $project -c Release -r win-x64 --self-contained false

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$publishDir = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin/Release/net9.0-windows10.0.19041.0/win-x64/publish"
$dll = Join-Path $publishDir "CoreVideoPro.WinUI.dll"
$exe = Join-Path $publishDir "CoreVideoPro.WinUI.exe"

if (-not (Test-Path $dll)) {
    $found = Get-ChildItem -Path (Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin") -Recurse -Filter "CoreVideoPro.WinUI.dll" |
        Where-Object { $_.DirectoryName -match "\\publish$" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($found) {
        $dll = $found.FullName
        $publishDir = $found.DirectoryName
        $exe = Join-Path $publishDir "CoreVideoPro.WinUI.exe"
    }
}

if (-not (Test-Path $dll)) {
    Write-Error "Could not find CoreVideoPro.WinUI.dll after build."
}

function Start-WinUiShell {
    param(
        [string]$PublishDirectory,
        [string]$DllPath,
        [string]$ExePath
    )

    if (Test-Path $ExePath) {
        try {
            Write-Host "Launching $ExePath"
            Start-Process -FilePath $ExePath -WorkingDirectory $PublishDirectory
            return
        }
        catch {
            $message = $_.Exception.Message
            if ($message -notmatch "side-by-side|configuration is incorrect|sxstrace") {
                throw
            }
            Write-Host "App host failed with side-by-side error; using dotnet host..." -ForegroundColor Yellow
        }
    }

    Write-Host "Launching dotnet $DllPath"
    Start-Process -FilePath "dotnet" -ArgumentList @($DllPath) -WorkingDirectory $PublishDirectory
}

Start-WinUiShell -PublishDirectory $publishDir -DllPath $dll -ExePath $exe