# Exercise the production catch path without running any live drill setup.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/outputs-drill-lib.ps1"
$tokens=$null; $parseErrors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile("$PSScriptRoot/outputs-drill.ps1",[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw 'Drill parse failed' }
$fn=$ast.Find({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Invoke-DrillQuery'},$true)
. ([scriptblock]::Create($fn.Extent.Text))
$api='http://example.invalid'
$secret='synthetic-password-1165'
$url="https://example.invalid/j/123?pwd=$secret"
function Invoke-RestMethod { param($Uri,$Method,$ContentType,$Body,$TimeoutSec)
  if ($script:succeed) { return @{ok=$true;data='unchanged'} }
  $e=[Exception]::new("Echoed request: $Body and secret synthetic-password-1165")
  $e | Add-Member NoteProperty Response ([pscustomobject]@{StatusCode=422})
  throw $e
}
$message=''
try { Invoke-DrillQuery 'zoom.join' @($url); throw 'unexpected success' } catch { $message=$_.Exception.Message }
if ($message -match [regex]::Escape($secret) -or $message -match 'https://' -or $message -notmatch 'zoom.join failed \(HTTP 422\)') { throw "Unsafe or unhelpful error: $message" }
if ((Format-DrillActionFailure $url 0) -notmatch '^control action failed \(transport failure\)') { throw 'Unsafe action text' }
$script:succeed=$true
$r=Invoke-DrillQuery 'scene.list'
if (!$r.ok -or $r.data -ne 'unchanged') { throw 'Success result changed' }
Write-Host 'PASS: actual query failure omits request/echoed secrets, preserves status; unsafe action and successful response covered.'
