# #883: Setup's previous-version cleanup must not depend on the environment it inherits.
#
# Reproduced 2026-10-23: a Windows PowerShell 5.1 child that inherits a PowerShell 7
# PSModulePath (PS7's own $PSHOME\Modules ahead of the Windows PowerShell paths) cannot
# resolve Get-FileHash or New-Guid: "The term 'Get-FileHash' is not recognized". Those are
# SCRIPT functions in 5.1's Microsoft.PowerShell.Utility, found by module auto-loading, and
# discovery binds to PS7's same-named Core-only module first. Compiled cmdlets
# (ConvertTo-Json, Get-ChildItem) keep working, which is why only one command failed. NSIS
# nsExec passes the environment through untouched, so an installer launched from a PS7
# shell hands the helper exactly this environment.
#
# This test runs the REAL Remove-PreviousInstallations.ps1 in a child host (64-bit System32
# and 32-bit SysWOW64, which is what the 32-bit NSIS installer launches) with polluted
# environments, against throwaway fixtures: a fake LOCALAPPDATA under %TEMP% and an HKCU
# test namespace. It never touches the real install root, the real uninstall keys or the
# real camera registration.
param(
    # Directory holding the helper under test (lets a reviewer point it at an older copy
    # to prove the test bites).
    [string]$HelperDirectory = $PSScriptRoot,
    # A real PowerShell 7 home to borrow Modules from. Default: the installed one if any.
    [string]$Ps7Home = ''
)
$ErrorActionPreference = 'Stop'
$ps64 = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$ps32 = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
if (![Environment]::Is64BitProcess -and [Environment]::Is64BitOperatingSystem) {
    # Running from the 32-bit host: System32 is redirected; Sysnative is the 64-bit host.
    $ps64 = Join-Path $env:WINDIR 'Sysnative\WindowsPowerShell\v1.0\powershell.exe'
}
$hosts = [ordered]@{ '64-bit' = $ps64 }
if (Test-Path -LiteralPath $ps32) { $hosts['32-bit'] = $ps32 } else { Write-Output 'SKIP: SysWOW64 PowerShell not present; 32-bit host not exercised.' }

$caseId = [Guid]::NewGuid().ToString('N')
$work = Join-Path ([IO.Path]::GetTempPath()) "CoreVideoRetirement-$caseId"
$testKey = "Software\CoreVideoProInstallerTests\$caseId"
$checks = 0
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message }; $script:checks++ }
function Get-Sha256([string]$Path) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { $s = [IO.File]::OpenRead($Path); try { return [BitConverter]::ToString($h.ComputeHash($s)).Replace('-', '') } finally { $s.Dispose() } } finally { $h.Dispose() }
}
function Open-Hkcu64 { [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64) }

# PowerShell 7.4's own Microsoft.PowerShell.Utility manifest, verbatim minus its signature
# block (MIT, github.com/PowerShell/PowerShell). Core-only; exports Get-FileHash as a cmdlet
# from a .NET assembly Windows PowerShell cannot load. A hand-trimmed manifest does NOT
# reproduce the fault (measured 2026-10-24), so keep this verbatim.
$fakePs7 = Join-Path $work 'fake-ps7'
$fakeUtility = Join-Path $fakePs7 'Modules\Microsoft.PowerShell.Utility'
New-Item -ItemType Directory -Force -Path $fakeUtility | Out-Null
[IO.File]::WriteAllText((Join-Path $fakeUtility 'Microsoft.PowerShell.Utility.psd1'), @'
@{
GUID = "1DA87E53-152B-403E-98DC-74D7B4D63D59"
Author = "PowerShell"
CompanyName = "Microsoft Corporation"
Copyright = "Copyright (c) Microsoft Corporation."
ModuleVersion = "7.0.0.0"
CompatiblePSEditions = @("Core")
PowerShellVersion = "3.0"
CmdletsToExport = @(
  'Export-Alias', 'Get-Alias', 'Import-Alias', 'New-Alias', 'Remove-Alias', 'Set-Alias', 'Export-Clixml', 'Import-Clixml',
  'Measure-Command', 'Trace-Command', 'ConvertFrom-Csv', 'ConvertTo-Csv', 'Export-Csv', 'Import-Csv', 'Get-Culture',
  'Format-Custom', 'Get-Date', 'Set-Date', 'Write-Debug', 'Wait-Debugger', 'Register-EngineEvent', 'Write-Error',
  'Get-Event', 'New-Event', 'Remove-Event', 'Unregister-Event', 'Wait-Event', 'Get-EventSubscriber', 'Invoke-Expression',
  'Out-File', 'Unblock-File', 'Get-FileHash', 'Export-FormatData', 'Get-FormatData', 'Update-FormatData', 'New-Guid',
  'Format-Hex', 'Get-Host', 'Read-Host', 'Write-Host', 'ConvertTo-Html', 'Write-Information', 'ConvertFrom-Json',
  'ConvertTo-Json', 'Test-Json', 'Format-List', 'Import-LocalizedData', 'Send-MailMessage', 'ConvertFrom-Markdown',
  'Show-Markdown', 'Get-MarkdownOption', 'Set-MarkdownOption', 'Add-Member', 'Get-Member', 'Compare-Object', 'Group-Object',
  'Measure-Object', 'New-Object', 'Select-Object', 'Sort-Object', 'Tee-Object', 'Register-ObjectEvent', 'Write-Output',
  'Import-PowerShellDataFile', 'Write-Progress', 'Disable-PSBreakpoint', 'Enable-PSBreakpoint', 'Get-PSBreakpoint',
  'Remove-PSBreakpoint', 'Set-PSBreakpoint', 'Get-PSCallStack', 'Export-PSSession', 'Import-PSSession', 'Get-Random', 'Get-SecureRandom'
  'Invoke-RestMethod', 'Debug-Runspace', 'Get-Runspace', 'Disable-RunspaceDebug', 'Enable-RunspaceDebug',
  'Get-RunspaceDebug', 'ConvertFrom-SddlString', 'Start-Sleep', 'Join-String', 'Out-String', 'Select-String',
  'ConvertFrom-StringData', 'Format-Table', 'New-TemporaryFile', 'New-TimeSpan', 'Get-TraceSource', 'Set-TraceSource',
  'Add-Type', 'Get-TypeData', 'Remove-TypeData', 'Update-TypeData', 'Get-UICulture', 'Get-Unique', 'Get-Uptime',
  'Clear-Variable', 'Get-Variable', 'New-Variable', 'Remove-Variable', 'Set-Variable', 'Get-Verb', 'Write-Verbose',
  'Write-Warning', 'Invoke-WebRequest', 'Format-Wide', 'ConvertTo-Xml', 'Select-Xml', 'Get-Error', 'Update-List',
  'Out-GridView', 'Show-Command', 'Out-Printer'
)
FunctionsToExport = @()
AliasesToExport = @('fhx')
NestedModules = @("Microsoft.PowerShell.Commands.Utility.dll")
HelpInfoURI = 'https://aka.ms/powershell73-help'
}
'@)
if (!$Ps7Home -and (Test-Path -LiteralPath "$env:ProgramW6432\PowerShell\7\Modules")) { $Ps7Home = "$env:ProgramW6432\PowerShell\7" }
$windowsPsPath = "$env:USERPROFILE\Documents\WindowsPowerShell\Modules;$env:ProgramW6432\WindowsPowerShell\Modules;$env:WINDIR\system32\WindowsPowerShell\v1.0\Modules"
$environments = [ordered]@{
    # What pwsh 7 exports to a child that is not powershell.exe (e.g. setup.exe).
    'ps7-inherited (7.4 manifest)' = "$env:USERPROFILE\Documents\PowerShell\Modules;$env:ProgramW6432\PowerShell\Modules;$fakePs7\Modules;$windowsPsPath"
    'empty' = $null
    'nonexistent' = 'C:\CoreVideoPro-883-does-not-exist'
}
if ($Ps7Home) { $environments['ps7-inherited (real PS7 modules)'] = "$env:USERPROFILE\Documents\PowerShell\Modules;$env:ProgramW6432\PowerShell\Modules;$Ps7Home\Modules;$windowsPsPath" }

function New-InstallFixture([string]$Root, [string]$Id, [string]$RegistryRoot) {
    $app = Join-Path $Root $Id
    New-Item -ItemType Directory -Path $app | Out-Null
    [IO.File]::WriteAllText((Join-Path $app '.corevideo-prerelease-install'), $Id)
    [IO.File]::WriteAllText((Join-Path $app 'app.exe'), "delivered payload $Id")
    $manifest = @{releaseId=$Id;sourceCommit=('a'*40);files=@(@{path='app.exe';sha256=(Get-Sha256 (Join-Path $app 'app.exe'))})}
    [IO.File]::WriteAllText((Join-Path $app 'build-manifest.json'), ($manifest | ConvertTo-Json -Depth 5))
    $b = Open-Hkcu64
    try {
        $k = $b.CreateSubKey("$RegistryRoot\CoreVideoPro-$Id")
        try { $k.SetValue('InstallLocation', $app); $k.SetValue('Publisher', 'CoreVideo Pro'); $k.SetValue('DisplayName', "CoreVideo Pro $Id") } finally { $k.Dispose() }
    } finally { $b.Dispose() }
    return $app
}

# Runs the helper in a child host with an exact environment, exactly as nsExec would.
function Invoke-Child([string]$HostExe, [hashtable]$Environment, [string]$CommandLine) {
    $psi = New-Object Diagnostics.ProcessStartInfo $HostExe, $CommandLine
    $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true   # a prompt must fail, not hang the test
    foreach ($name in $Environment.Keys) {
        if ($null -eq $Environment[$name]) { [void]$psi.EnvironmentVariables.Remove($name) } else { $psi.EnvironmentVariables[$name] = $Environment[$name] }
    }
    $p = [Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    if (!$p.WaitForExit(180000)) { $p.Kill(); throw "Child timed out after 180 s: $CommandLine" }
    $out = $outTask.Result
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $out; Err = $errTask.Result }
}
function Invoke-Helper([string]$HostExe, [hashtable]$Environment, [string[]]$Arguments) {
    $quoted = @($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }) -join ' '
    if (!$quoted) { throw "Invoke-Helper got no arguments ($($Arguments.Count))" }
    return Invoke-Child $HostExe $Environment "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$(Join-Path $HelperDirectory 'Remove-PreviousInstallations.ps1')`" $quoted"
}

# One fixture world per run: fake LOCALAPPDATA, current + one registered old install.
$script:world = 0
function New-World([string]$HostExe) {
    $script:world++
    $local = Join-Path $work "lad-$($script:world)"
    $root = Join-Path $local 'Programs\CoreVideoPro'
    New-Item -ItemType Directory -Force -Path $root, (Join-Path $local 'StartMenu') | Out-Null
    $registry = "$testKey\w$($script:world)\Uninstall"
    $current = New-InstallFixture $root 'beta-2026-10-23-current' $registry
    $old = New-InstallFixture $root 'beta-2026-10-22-old' $registry
    $camera = "$testKey\w$($script:world)\Camera"
    $b = Open-Hkcu64
    try { $k = $b.CreateSubKey($camera); try { $k.SetValue('CoreVideoOwnerRole', 'CoreVideoPro.VirtualCamera.v1'); $k.SetValue('CoreVideoOwnerAppDirectory', $current) } finally { $k.Dispose() } } finally { $b.Dispose() }
    return [pscustomobject]@{ Local=$local; Root=$root; Current=$current; Old=$old; Registry=$registry; Camera=$camera
        Args=@('-AppDirectory', $current, '-RegistryRoot', "HKCU:\$registry", '-ProgramsDirectory', (Join-Path $local 'StartMenu'), '-CameraRegistrationKey', $camera) }
}
function Test-RegistryKey([string]$Path) { $b = Open-Hkcu64; try { $k = $b.OpenSubKey($Path); if ($k) { $k.Dispose(); return $true }; return $false } finally { $b.Dispose() } }

New-Item -ItemType Directory -Force -Path $work | Out-Null
$results = [Collections.Generic.List[object]]::new()
try {
    foreach ($hostName in $hosts.Keys) {
        foreach ($envName in $environments.Keys) {
            $label = "[$hostName, PSModulePath=$envName]"
            $w = New-World $hosts[$hostName]
            $r = Invoke-Helper $hosts[$hostName] @{ LOCALAPPDATA = $w.Local; PSModulePath = $environments[$envName]; PSModuleAnalysisCachePath = (Join-Path $w.Local 'ModuleAnalysisCache') } $w.Args  # private analysis cache: never touch the operator's
            Assert ($r.Code -eq 0) "$label helper exited $($r.Code). stderr: $($r.Err) stdout: $($r.Out)"
            Assert ($r.Err -notmatch 'is not recognized') "$label a command did not resolve: $($r.Err)"
            Assert (!(Test-Path -LiteralPath $w.Old)) "$label superseded fixture folder survived: $($r.Out)"
            Assert (Test-Path -LiteralPath (Join-Path $w.Current 'app.exe')) "$label current install changed."
            Assert (!(Test-RegistryKey "$($w.Registry)\CoreVideoPro-beta-2026-10-22-old")) "$label superseded registration survived."
            $report = Get-Content -LiteralPath (Join-Path $w.Current 'previous-installations-cleanup.json') -Raw | ConvertFrom-Json
            $entries = @($report)
            if ($report.PSObject.Properties.Name -contains 'registered') { $entries = @($report.registered) }
            Assert (@($entries | Where-Object { $_.releaseId -eq 'beta-2026-10-22-old' -and $_.status -eq 'removed' }).Count -eq 1) "$label cleanup report does not show the old version removed."
            # Task 2: the pinned environment is logged on every run, inherited value included.
            $log = Join-Path $w.Local 'CoreVideoPro\installer-logs\beta-2026-10-23-current-retire.log'
            Assert (Test-Path -LiteralPath $log) "$label retire log not written: $log"
            $logText = Get-Content -LiteralPath $log -Raw
            Assert ($logText -match [regex]::Escape("is64Bit=$($hostName -eq '64-bit')")) "$label log lacks host bitness: $logText"
            Assert ($logText -match 'languageMode=FullLanguage') "$label log lacks language mode."
            Assert ($logText -match [regex]::Escape("psModulePathEffective=$(Join-Path $(if ($hostName -eq '32-bit') { "$env:WINDIR\SysWOW64" } else { "$env:WINDIR\System32" }) 'WindowsPowerShell\v1.0\Modules')")) "$label effective PSModulePath not pinned to `$PSHOME\Modules: $logText"
            # 5.1 itself edits the inherited value at startup (the 32-bit host inserts the x86
            # Program Files path), so match the polluting entries, not the whole string.
            foreach ($entry in @("$($environments[$envName])".Split(';') | Where-Object { $_ -match 'PowerShell\\7|fake-ps7|does-not-exist' })) {
                Assert ($logText -match ('psModulePathInherited=[^\r\n]*' + [regex]::Escape($entry))) "$label inherited PSModulePath entry $entry not logged: $logText"
            }
            $results.Add("$label ok")
        }
        # Task 2: an unresolvable command fails loudly, by name, with diagnostics, and is logged.
        $label = "[$hostName, unresolvable command]"
        $w = New-World $hosts[$hostName]
        $r = Invoke-Helper $hosts[$hostName] @{ LOCALAPPDATA = $w.Local; PSModuleAnalysisCachePath = (Join-Path $w.Local 'ModuleAnalysisCache') } ($w.Args + @('-RequiredCommands', 'Get-CoreVideo883Missing'))
        $text = "$($r.Out)`n$($r.Err)"
        Assert ($r.Code -ne 0) "$label helper did not fail: $text"
        Assert ($text -match 'cannot resolve Get-CoreVideo883Missing') "$label failure does not name the command: $text"
        foreach ($field in 'is64Bit=','languageMode=','psModulePathEffective=','psHome=') { Assert ($text -match $field) "$label failure lacks $field" }
        Assert (Test-Path -LiteralPath $w.Old) "$label a failed environment check still retired a version."
        $log = Join-Path $w.Local 'CoreVideoPro\installer-logs\beta-2026-10-23-current-retire.log'
        Assert ((Test-Path -LiteralPath $log) -and ((Get-Content -LiteralPath $log -Raw) -match 'FAILED: .*Get-CoreVideo883Missing.*psModulePathEffective=')) "$label failure not logged with diagnostics."
        $results.Add("$label ok")

        # Task 5 end to end: registered + residual passes, honest report, loud backup path.
        $label = "[$hostName, residual sweep end to end]"
        $w = New-World $hosts[$hostName]
        foreach ($id in 'beta-2026-10-01-meta1', 'beta-2026-10-02-meta2', 'beta-2026-10-03-meta3') {
            New-Item -ItemType Directory -Path (Join-Path $w.Root $id) | Out-Null
            [IO.File]::WriteAllText((Join-Path $w.Root "$id\.camera-desktop-registration-before.json"), '{}')
        }
        $rec = Join-Path $w.Root 'beta-2026-10-04-recordings\Recordings\CoreVideo Pro'
        New-Item -ItemType Directory -Force -Path $rec | Out-Null
        [IO.File]::WriteAllText((Join-Path $rec 'take.mp4'), 'operator recording')
        $r = Invoke-Helper $hosts[$hostName] @{ LOCALAPPDATA = $w.Local; PSModuleAnalysisCachePath = (Join-Path $w.Local 'ModuleAnalysisCache') } $w.Args
        Assert ($r.Code -eq 2) "$label expected exit 2 (done, operator files moved); got $($r.Code). $($r.Out) $($r.Err)"
        $left = @(Get-ChildItem -LiteralPath $w.Root -Directory -Force | ForEach-Object Name)
        Assert ($left.Count -eq 1 -and $left[0] -eq 'beta-2026-10-23-current') "$label folders left: $($left -join ', ')"
        $backups = @(Get-ChildItem -LiteralPath (Join-Path $w.Local 'CoreVideoPro\backups') -Directory -Filter 'retired-install-remnants-*')
        Assert ($backups.Count -eq 1) "$label expected one dated backup folder."
        Assert ((Get-Content -LiteralPath (Join-Path $backups[0].FullName 'beta-2026-10-04-recordings\Recordings\CoreVideo Pro\take.mp4') -Raw) -eq 'operator recording') "$label recording not in the backup."
        $report = Get-Content -LiteralPath (Join-Path $w.Current 'previous-installations-cleanup.json') -Raw | ConvertFrom-Json
        $s = $report.summary
        Assert ($s.registeredRetired -eq 1 -and $s.residualRemoved -eq 3 -and $s.residualBackedUp -eq 1 -and $s.retained -eq 0) "$label summary wrong: $($s | ConvertTo-Json -Compress)"
        Assert ($s.backupFolder -eq $backups[0].FullName) "$label summary lacks the backup folder."
        Assert (@($report.residual).Count -eq 4 -and @($report.registered).Count -eq 1) "$label report lists are wrong."
        $lines = @($r.Out -split "`r?`n" | Where-Object { $_.Trim() })
        Assert ($lines[-1] -match '^Previous installation cleanup: 4 retired, 1 backed up, 0 retained\.') "$label last line: $($lines[-1])"
        Assert ($r.Out -match [regex]::Escape($backups[0].FullName)) "$label backup path not reported on stdout."
        Assert (Test-Path -LiteralPath (Join-Path $w.Local 'CoreVideoPro\installer-logs\beta-2026-10-23-current-retire.json')) "$label report copy not kept in installer-logs."
        $results.Add("$label ok")

        # A locked operator file: exit 1, folder kept, the last line counts it.
        $label = "[$hostName, residual sweep locked file]"
        $w = New-World $hosts[$hostName]
        $rec = Join-Path $w.Root 'beta-2026-10-04-recordings\Recordings'
        New-Item -ItemType Directory -Force -Path $rec | Out-Null
        [IO.File]::WriteAllText((Join-Path $rec 'take.mp4'), 'in use')
        [IO.File]::WriteAllText((Join-Path $rec 'a-first.mp4'), 'moves before the locked one')
        # Share READ (as a player holding the file open would): hashing succeeds, the move fails
        # after a-first.mp4 has already moved. That is the partial-move case.
        $lock = [IO.File]::Open((Join-Path $rec 'take.mp4'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try { $r = Invoke-Helper $hosts[$hostName] @{ LOCALAPPDATA = $w.Local; PSModuleAnalysisCachePath = (Join-Path $w.Local 'ModuleAnalysisCache') } $w.Args } finally { $lock.Dispose() }
        $lines = @($r.Out -split "`r?`n" | Where-Object { $_.Trim() })
        Assert ($r.Code -eq 1) "$label expected exit 1; got $($r.Code). $($r.Out) $($r.Err)"
        Assert ($lines[-1] -match '1 retained') "$label last line: $($lines[-1])"
        Assert ((Get-Content -LiteralPath (Join-Path $rec 'take.mp4') -Raw) -eq 'in use') "$label locked file changed."
        # Partial move, then failure: the warning must still name the backup (ruling D4),
        # ahead of the Retained lines because NSIS keeps only the head of stdout.
        $report = Get-Content -LiteralPath (Join-Path $w.Current 'previous-installations-cleanup.json') -Raw | ConvertFrom-Json
        $backups = @(Get-ChildItem -LiteralPath (Join-Path $w.Local 'CoreVideoPro\backups') -Directory -Filter 'retired-install-remnants-*')
        Assert ($backups.Count -eq 1 -and $report.summary.backupFolder -eq $backups[0].FullName -and $report.summary.filesBackedUp -eq 1) "$label JSON lacks the partial backup: $($report.summary | ConvertTo-Json -Compress)"
        Assert ((Get-Content -LiteralPath (Join-Path $backups[0].FullName 'beta-2026-10-04-recordings\Recordings\a-first.mp4') -Raw) -eq 'moves before the locked one') "$label moved file not in the backup."
        $movedAt = [array]::FindIndex([string[]]$lines, [Predicate[string]]{ param($l) $l -match 'MOVED, not deleted, to: ' + [regex]::Escape($backups[0].FullName) })
        $retainedAt = [array]::FindIndex([string[]]$lines, [Predicate[string]]{ param($l) $l -like 'Retained *' })
        Assert ($movedAt -ge 0 -and $retainedAt -gt $movedAt) "$label stdout must name the backup before the Retained lines: $($r.Out)"
        $results.Add("$label ok")
    }
    [ordered]@{ passed=$true; checks=$checks; cases=@($results); evidence=$work } | ConvertTo-Json
} finally {
    # Only this run's key, in the dedicated non-product test namespace.
    $b = Open-Hkcu64
    try { $b.DeleteSubKeyTree($testKey, $false) } finally { $b.Dispose() }
}
