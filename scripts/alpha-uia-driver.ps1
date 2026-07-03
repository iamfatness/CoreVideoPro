# Alpha validation UIA driver for the CoreVideo Pro WinUI shell.
# Stateless per invocation: finds the app window fresh each call.
#
# Actions:
#   -Action dump [-Scope all|buttons|combos|edits] [-MaxDepth n]
#   -Action restore                       # gentle SW_RESTORE (window opens minimized off-screen)
#   -Action invoke -Name "Join Zoom"      # Button InvokePattern (exact name)
#   -Action toggle -Name "..."            # Button fallback: Invoke or Toggle pattern
#   -Action setvalue -Name "Zoom meeting URL or ID" -Value "..."   # Edit ValuePattern
#   -Action select -ComboIndex 3 -ItemName "Zoom"                  # nth ComboBox on screen
#   -Action check -CheckIndex 1           # nth CheckBox -> ensure checked
#   -Action screenshot -Path out.png
#   -Action text                          # dump all visible TextBlock names (status readouts)
param(
  [Parameter(Mandatory = $true)][string]$Action,
  [string]$Name,
  [string]$Value,
  [int]$ComboIndex = -1,
  [string]$ItemName,
  [int]$CheckIndex = -1,
  [string]$Path,
  [string]$Scope = "all",
  [int]$MaxDepth = 12,
  [string]$ProcessName = "CoreVideoPro.WinUI"
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing | Out-Null
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AlphaUser32 {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { throw "Process $ProcessName not running." }
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
  # Window may be off-screen minimized; enumerate top-level windows by pid via UIA.
  $rootAll = [System.Windows.Automation.AutomationElement]::RootElement
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $proc.Id)
  $win = $rootAll.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
  if ($win) { $hwnd = [IntPtr]$win.Current.NativeWindowHandle }
}
if ($hwnd -eq [IntPtr]::Zero) { throw "No window handle for $ProcessName." }

if ($Action -eq "restore") {
  [AlphaUser32]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE, gentle
  Start-Sleep -Milliseconds 700
  $r = New-Object AlphaUser32+RECT
  [AlphaUser32]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  Write-Output ("rect={0},{1},{2},{3}" -f $r.Left, $r.Top, $r.Right, $r.Bottom)
  exit 0
}

$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
if (-not $root) { throw "UIA root not found." }

function Find-All([System.Windows.Automation.AutomationElement]$el, $controlType) {
  $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $controlType)
  return $el.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Find-ByName([System.Windows.Automation.AutomationElement]$el, $controlType, [string]$name) {
  $c1 = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $controlType)
  $c2 = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $name)
  $and = New-Object System.Windows.Automation.AndCondition($c1, $c2)
  return $el.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $and)
}

switch ($Action) {
  "dump" {
    $types = @{
      "buttons" = @([System.Windows.Automation.ControlType]::Button)
      "combos"  = @([System.Windows.Automation.ControlType]::ComboBox)
      "edits"   = @([System.Windows.Automation.ControlType]::Edit)
      "all"     = @([System.Windows.Automation.ControlType]::Button, [System.Windows.Automation.ControlType]::ComboBox,
                    [System.Windows.Automation.ControlType]::Edit, [System.Windows.Automation.ControlType]::CheckBox,
                    [System.Windows.Automation.ControlType]::ListItem, [System.Windows.Automation.ControlType]::Text)
    }[$Scope]
    foreach ($t in $types) {
      foreach ($e in (Find-All $root $t)) {
        $cur = $e.Current
        if ($cur.IsOffscreen) { continue }
        Write-Output ("{0}`t{1}`t{2}`t({3:0},{4:0})" -f $cur.ControlType.ProgrammaticName.Replace("ControlType.", ""), $cur.Name, $cur.AutomationId, $cur.BoundingRectangle.X, $cur.BoundingRectangle.Y)
      }
    }
  }
  "text" {
    foreach ($e in (Find-All $root ([System.Windows.Automation.ControlType]::Text))) {
      $cur = $e.Current
      if (-not $cur.IsOffscreen -and $cur.Name) { Write-Output $cur.Name }
    }
  }
  "invoke" {
    $el = Find-ByName $root ([System.Windows.Automation.ControlType]::Button) $Name
    if (-not $el) { throw "Button '$Name' not found." }
    $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Write-Output "invoked:$Name"
  }
  "toggle" {
    $el = Find-ByName $root ([System.Windows.Automation.ControlType]::Button) $Name
    if (-not $el) { throw "Button '$Name' not found." }
    $p = $null
    if ($el.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke() }
    else {
      $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
    }
    Write-Output "toggled:$Name"
  }
  "setvalue" {
    $el = Find-ByName $root ([System.Windows.Automation.ControlType]::Edit) $Name
    if (-not $el) { throw "Edit '$Name' not found." }
    $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Value)
    Write-Output "set:$Name=$Value"
  }
  "select" {
    $combos = Find-All $root ([System.Windows.Automation.ControlType]::ComboBox)
    $visible = @()
    foreach ($c in $combos) { if (-not $c.Current.IsOffscreen) { $visible += $c } }
    if ($ComboIndex -lt 0 -or $ComboIndex -ge $visible.Count) { throw "ComboIndex $ComboIndex out of range (0..$($visible.Count - 1))." }
    $combo = $visible[$ComboIndex]
    $exp = $combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $exp.Expand()
    Start-Sleep -Milliseconds 500
    $item = Find-ByName $combo ([System.Windows.Automation.ControlType]::ListItem) $ItemName
    if (-not $item) {
      # popup may host items outside the combo subtree; search the window
      $item = Find-ByName $root ([System.Windows.Automation.ControlType]::ListItem) $ItemName
    }
    if (-not $item) { $exp.Collapse(); throw "Item '$ItemName' not found in combo $ComboIndex." }
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 200
    try { $exp.Collapse() } catch {}
    Write-Output "selected:[$ComboIndex]=$ItemName"
  }
  "check" {
    $checks = Find-All $root ([System.Windows.Automation.ControlType]::CheckBox)
    $visible = @()
    foreach ($c in $checks) { if (-not $c.Current.IsOffscreen) { $visible += $c } }
    if ($CheckIndex -lt 0 -or $CheckIndex -ge $visible.Count) { throw "CheckIndex $CheckIndex out of range (0..$($visible.Count - 1))." }
    $t = $visible[$CheckIndex].GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    if ($t.Current.ToggleState -ne [System.Windows.Automation.ToggleState]::On) { $t.Toggle() }
    Write-Output ("check:[{0}]={1}" -f $CheckIndex, $t.Current.ToggleState)
  }
  "screenshot" {
    if (-not $Path) { throw "screenshot needs -Path" }
    $r = New-Object AlphaUser32+RECT
    [AlphaUser32]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { throw "window rect empty ($($r.Left),$($r.Top),$($r.Right),$($r.Bottom))" }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "screenshot:$Path ($w x $h)"
  }
  default { throw "Unknown action $Action" }
}
