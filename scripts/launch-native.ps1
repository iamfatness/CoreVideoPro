$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj"

Write-Host "Building CoreVideo Pro (WinUI native shell)..."
dotnet publish $project -c Release -r win-x64 --self-contained false

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$publishDir = Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin/Release/net9.0-windows10.0.19041.0/win-x64/publish"
$exe = Join-Path $publishDir "CoreVideoPro.WinUI.exe"
$dll = Join-Path $publishDir "CoreVideoPro.WinUI.dll"

if (-not (Test-Path $exe)) {
    $found = Get-ChildItem -Path (Join-Path $repoRoot "native-shell/CoreVideoPro.WinUI/bin") -Recurse -Filter "CoreVideoPro.WinUI.exe" |
        Where-Object { $_.DirectoryName -match "\\publish$" } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($found) {
        $exe = $found.FullName
        $publishDir = $found.DirectoryName
        $dll = Join-Path $publishDir "CoreVideoPro.WinUI.dll"
    }
}

if (-not (Test-Path $dll)) {
    Write-Error "Could not find CoreVideoPro.WinUI.dll after build."
}

function Start-WinUiShell {
    param(
        [string]$PublishDirectory,
        [string]$ExePath,
        [string]$DllPath
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
            Write-Host "App host failed with side-by-side error; retrying via dotnet host..." -ForegroundColor Yellow
        }
    }

    if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
        throw "WinUI app host failed and dotnet is not on PATH. Install Windows App Runtime 2.x from https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads"
    }

    Write-Host "Launching dotnet $DllPath"
    Start-Process -FilePath "dotnet" -ArgumentList @($DllPath) -WorkingDirectory $PublishDirectory
}

Start-WinUiShell -PublishDirectory $publishDir -ExePath $exe -DllPath $dll