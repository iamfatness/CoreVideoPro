# Stop CoreVideo Pro desktop dev processes (Electron + Vite on common dev ports).
param(
  [int[]]$Ports = @(5173, 5174, 5175, 5176, 5177, 5178, 5179, 5180)
)

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Write-Host "Stopping CoreVideo Pro desktop dev from $repoRoot"

Get-Process electron -ErrorAction SilentlyContinue | ForEach-Object {
  $cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
  if ($cmd -and ($cmd -like "*$repoRoot*" -or $cmd -like "*CoreVideoPro*")) {
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    Write-Host "Stopped electron ($($_.Id))"
  }
}

foreach ($port in $Ports) {
  $procIds = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty OwningProcess -Unique
  foreach ($procId in $procIds) {
    if (-not $procId) { continue }
    $cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$procId" -ErrorAction SilentlyContinue).CommandLine
    if ($cmd -and ($cmd -like "*vite*" -or $cmd -like "*CoreVideoPro*" -or $cmd -like "*corevideo-pro*")) {
      Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
      Write-Host "Stopped vite/listener on port $port (pid $procId)"
    }
  }
}

Write-Host "Done."