$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj"

Write-Host "Building CoreVideo Pro (WinUI native shell)..."
dotnet publish $project -c Release -r win-x64 --self-contained false

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$exe = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish\CoreVideoPro.WinUI.exe"
if (-not (Test-Path $exe)) {
    $exe = Get-ChildItem -Path (Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\bin") -Recurse -Filter "CoreVideoPro.WinUI.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

if (-not $exe -or -not (Test-Path $exe)) {
    Write-Error "Could not find CoreVideoPro.WinUI.exe after build."
}

Write-Host "Launching $exe"
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)