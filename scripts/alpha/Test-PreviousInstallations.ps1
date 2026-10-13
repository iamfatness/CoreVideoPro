param([switch]$WithoutHashCommand)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PreviousInstallations.psm1') -Force
if ($WithoutHashCommand) {
    # Mask command lookup in the actual planner's module scope, not the test
    # script's separate session state. Run this mode in a disposable -File process.
    & (Get-Module PreviousInstallations) {
        function script:Get-FileHash { throw 'Get-FileHash unavailable in installer helper' }
    }
}
$caseId = [Guid]::NewGuid().ToString('N')
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) "CoreVideoRetirement-$caseId"
$registryRoot = "HKCU:/Software/CoreVideoProInstallerTests/$caseId"
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
New-Item -Path $registryRoot -Force | Out-Null
$checks = 0
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message }; $script:checks++ }
function New-Fixture([string]$Id) {
    $app = Join-Path $fixtureRoot $Id
    New-Item -ItemType Directory -Path $app | Out-Null
    [IO.File]::WriteAllText((Join-Path $app '.corevideo-prerelease-install'), $Id)
    [IO.File]::WriteAllText((Join-Path $app 'app.exe'), 'delivered payload')
    $hash = (Microsoft.PowerShell.Utility\Get-FileHash (Join-Path $app 'app.exe')).Hash
    @{releaseId=$Id;sourceCommit=('a'*40);files=@(@{path='app.exe';sha256=$hash})} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $app 'build-manifest.json')
    $key = Join-Path $registryRoot "CoreVideoPro-$Id"
    New-Item -Path $key | Out-Null
    Set-ItemProperty -LiteralPath $key -Name InstallLocation -Value $app
    Set-ItemProperty -LiteralPath $key -Name Publisher -Value 'CoreVideo Pro'
    Set-ItemProperty -LiteralPath $key -Name DisplayName -Value "CoreVideo Pro $Id"
    return $app
}
try {
    $current = New-Fixture 'beta-2026-10-12-current'
    $old = New-Fixture 'beta-2026-10-11-old'
    [IO.File]::WriteAllText((Join-Path $old 'recording.mov'), 'operator recording')
    $arguments = @{CurrentDirectory=$current;InstallRoot=$fixtureRoot;RegistryRoot=$registryRoot;RunningPaths=@()}
    $plan = @(Get-PreviousInstallationPlan @arguments)
    Assert ($plan.Count -eq 1 -and $plan[0].status -eq 'ready') 'Expected one superseded candidate.'
    $result = Remove-PreviousInstallation $plan[0]
    Assert ($result.status -eq 'removed') 'Owned removal failed.'
    Assert (!(Test-Path (Join-Path $old 'app.exe'))) 'Old payload survived.'
    Assert ((Get-Content (Join-Path $old 'recording.mov') -Raw) -eq 'operator recording') 'User recording was removed.'
    Assert (Test-Path (Join-Path $current 'app.exe')) 'Current version was removed.'
    Assert (!(Test-Path (Join-Path $registryRoot 'CoreVideoPro-beta-2026-10-11-old'))) 'Old registry entry survived.'

    $running = New-Fixture 'beta-2026-10-11-running'
    $arguments.RunningPaths = @((Join-Path $running 'app.exe'))
    $plan = @(Get-PreviousInstallationPlan @arguments)
    Assert ($plan[0].status -eq 'retained' -and $plan[0].reason -match 'running') 'Running version was not retained.'
    $arguments.RunningPaths = @()
    [IO.File]::WriteAllText((Join-Path $running '.corevideo-prerelease-install'), 'wrong marker')
    $plan = @(Get-PreviousInstallationPlan @arguments)
    Assert ($plan[0].status -eq 'retained') 'Wrong marker accepted.'

    $unsafe = New-Fixture 'beta-2026-10-11-unsafe'
    $manifest = Get-Content (Join-Path $unsafe 'build-manifest.json') -Raw | ConvertFrom-Json
    $manifest.files[0].path = '../escape.mov'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $unsafe 'build-manifest.json')
    $plan = @(Get-PreviousInstallationPlan @arguments)
    Assert (($plan | Where-Object directory -eq $unsafe).status -eq 'retained') 'Path traversal accepted.'
    Assert (Test-Path (Join-Path $unsafe 'app.exe')) 'Unsafe candidate changed.'

    $modified = New-Fixture 'beta-2026-10-11-modified'
    [IO.File]::WriteAllText((Join-Path $modified 'app.exe'), 'user modified content')
    $plan = @(Get-PreviousInstallationPlan @arguments)
    $result = Remove-PreviousInstallation ($plan | Where-Object directory -eq $modified)
    Assert ($result.status -eq 'removed' -and (Test-Path (Join-Path $modified 'app.exe'))) 'Modified file was not preserved.'

    $stale = New-Fixture 'beta-2026-10-11-stale'
    # Explicitly checked, owned fixture files only; no recursive deletion.
    Get-ChildItem -LiteralPath $stale -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    Remove-Item -LiteralPath $stale
    $plan = @(Get-PreviousInstallationPlan @arguments)
    $result = Remove-PreviousInstallation ($plan | Where-Object directory -eq $stale)
    Assert ($result.status -eq 'removed') 'Stale entry not retired.'

    $runtime = New-Fixture 'beta-2026-10-11-runtime'
    New-Item -ItemType Directory -Path (Join-Path $runtime 'notices/ffmpeg') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $runtime 'avdevice-62.dll'), 'runtime')
    [IO.File]::WriteAllText((Join-Path $runtime 'notices/ffmpeg/LICENSE.txt'), 'license')
    [IO.File]::WriteAllText((Join-Path $runtime 'notices/ffmpeg/user-notes.txt'), 'operator notes')
    [IO.File]::WriteAllLines((Join-Path $runtime 'notices/ffmpeg/installed-files.txt'), [string[]]@('avdevice-62.dll','notices\\ffmpeg\\LICENSE.txt'))
    $plan = @(Get-PreviousInstallationPlan @arguments)
    $result = Remove-PreviousInstallation ($plan | Where-Object directory -eq $runtime)
    Assert ($result.status -eq 'removed' -and !(Test-Path (Join-Path $runtime 'avdevice-62.dll'))) 'Media runtime not retired.'
    Assert (Test-Path (Join-Path $runtime 'notices/ffmpeg/user-notes.txt')) 'Unknown runtime-directory user file removed.'

    $locked = New-Fixture 'beta-2026-10-11-locked'
    $plan = @(Get-PreviousInstallationPlan @arguments)
    $lock = [IO.File]::Open((Join-Path $locked 'app.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { $result = Remove-PreviousInstallation ($plan | Where-Object directory -eq $locked) } finally { $lock.Dispose() }
    Assert ($result.status -eq 'retained') 'Locked file not reported.'
    Assert (Test-Path (Join-Path $locked 'build-manifest.json')) 'Failed cleanup destroyed retry manifest.'
    Assert (Test-Path (Join-Path $registryRoot 'CoreVideoPro-beta-2026-10-11-locked')) 'Failed cleanup removed registration.'

    $linked = New-Fixture 'beta-2026-10-11-linked'
    New-Item -ItemType Junction -Path (Join-Path $linked 'redirect') -Target $current | Out-Null
    $manifest = Get-Content (Join-Path $linked 'build-manifest.json') -Raw | ConvertFrom-Json
    $manifest.files[0].path = 'redirect/app.exe'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $linked 'build-manifest.json')
    $plan = @(Get-PreviousInstallationPlan @arguments)
    Assert (($plan | Where-Object directory -eq $linked).status -eq 'retained') 'Junction candidate accepted.'
    Assert (Test-Path (Join-Path $current 'app.exe')) 'Junction test altered current install.'

    Remove-Item -LiteralPath (Join-Path $current '.corevideo-prerelease-install')
    $refused = $false
    try { Get-PreviousInstallationPlan @arguments | Out-Null } catch { $refused = $true }
    Assert $refused 'Failed/uncommitted new install did not refuse retirement.'
    Assert (Test-Path (Join-Path $running 'app.exe')) 'Failed new install changed previous install.'
    [ordered]@{passed=$true;checks=$checks;evidence=$fixtureRoot} | ConvertTo-Json
} finally {
    # Only this fresh test key, in a dedicated non-product namespace.
    $resolvedKey = Get-Item -LiteralPath $registryRoot
    if ($resolvedKey.PSChildName -ne $caseId) { throw 'Unexpected test registry cleanup target.' }
    Remove-Item -LiteralPath $registryRoot -Recurse
}
