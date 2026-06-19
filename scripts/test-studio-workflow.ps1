param(
  [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$studioExe = Join-Path $repoRoot "studio\build-clean\Debug\CoreVideoStudio.exe"

if (-not $NoBuild -or -not (Test-Path $studioExe)) {
  & (Join-Path $PSScriptRoot "build-studio.ps1")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $studioExe)) {
  throw "CoreVideoStudio.exe not found at $studioExe"
}

$process = Start-Process -FilePath $studioExe -WorkingDirectory $repoRoot -PassThru
Start-Sleep -Seconds 2

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class CoreVideoStudioSmokeUser32 {
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr GetDlgItem(IntPtr hDlg, int nIDDlgItem);
  [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessage(IntPtr hWnd, int Msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern int SendMessage(IntPtr hWnd, int Msg, int wParam, StringBuilder lParam);
}
'@

function Get-ControlText([IntPtr]$Handle) {
  $length = [CoreVideoStudioSmokeUser32]::SendMessage($Handle, 0x000E, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32()
  $text = New-Object System.Text.StringBuilder ($length + 2)
  [CoreVideoStudioSmokeUser32]::SendMessage($Handle, 0x000D, $text.Capacity, $text) | Out-Null
  return $text.ToString()
}

try {
  $window = [CoreVideoStudioSmokeUser32]::FindWindow("CoreVideoStudioNativeWindow", "CoreVideo Pro - Native Studio")
  if ($window -eq [IntPtr]::Zero) {
    throw "CoreVideo Studio window not found."
  }

  foreach ($commandId in @(1002, 1007, 1008, 1004, 1005)) {
    [CoreVideoStudioSmokeUser32]::SendMessage($window, 0x0111, [IntPtr]$commandId, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 900
  }
  Start-Sleep -Seconds 2

  if ($process.HasExited) {
    throw "CoreVideo Studio exited early with code $($process.ExitCode)."
  }

  $participants = Get-ControlText ([CoreVideoStudioSmokeUser32]::GetDlgItem($window, 2005))
  $health = Get-ControlText ([CoreVideoStudioSmokeUser32]::GetDlgItem($window, 2006))

  if ($participants -notmatch "CoreVideo Producer" -or $participants -notmatch "Guest 1") {
    throw "Participant roster did not update.`n$participants"
  }
  if ($health -notmatch "Zoom: in_meeting" -or $health -notmatch "Output: live" -or $health -notmatch "Recording:") {
    throw "Health panel did not reach expected live recording state.`n$health"
  }
  if (-not (Get-Process corevideo-native -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    throw "corevideo-native.exe was not running after workflow commands."
  }

  Write-Host "[test-studio-workflow] native Studio workflow passed." -ForegroundColor Green
  Write-Host $participants
  Write-Host $health
}
finally {
  Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  Get-Process corevideo-native -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
