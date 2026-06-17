$ErrorActionPreference = "Stop"
$publishDir = Join-Path $PSScriptRoot "..\native-shell\CoreVideoPro.WinUI\bin\Release\net9.0-windows10.0.19041.0\win-x64\publish"
$exe = Join-Path $publishDir "CoreVideoPro.WinUI.exe"
$traceLog = Join-Path $env:TEMP "corevideo-sxs.etl"
$parseOut = Join-Path $env:TEMP "corevideo-sxs.txt"

$sxstrace = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter sxstrace.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if (-not $sxstrace) {
    throw "sxstrace.exe not found. Install Windows SDK."
}

if (Test-Path $traceLog) { Remove-Item $traceLog -Force }
if (Test-Path $parseOut) { Remove-Item $parseOut -Force }

& $sxstrace.FullName trace -logfile:$traceLog | Out-Null
try {
    Start-Process -FilePath $exe -WorkingDirectory $publishDir -Wait -ErrorAction Stop | Out-Null
}
catch {
    Write-Host $_.Exception.Message
}
& $sxstrace.FullName parse -logfile:$traceLog -outfile:$parseOut | Out-Null
Get-Content $parseOut