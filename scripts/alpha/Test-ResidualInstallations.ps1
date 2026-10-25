# #883: residual version folders (no uninstall registration) under the install root.
# Classification (Get-ResidualInstallationPlan) is read-only; removal/backup
# (Remove-ResidualInstallation) moves operator content, never deletes it (owner ruling D4).
# Fixtures live under %TEMP%\CoreVideoRetirement-<guid> and an HKCU test namespace only.
# Run from both the 64-bit and the 32-bit (SysWOW64) Windows PowerShell hosts.
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PreviousInstallations.psm1') -Force
$caseId = [Guid]::NewGuid().ToString('N')
$work = Join-Path ([IO.Path]::GetTempPath()) "CoreVideoRetirement-$caseId"
$root = Join-Path $work 'Programs\CoreVideoPro'
$registryRoot = "HKCU:\Software\CoreVideoProInstallerTests\$caseId"
New-Item -ItemType Directory -Force -Path $root | Out-Null
New-Item -Path $registryRoot -Force | Out-Null
$checks = 0
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message }; $script:checks++ }
function Get-Sha256([string]$Path) { Get-InstallPayloadSha256 -Path $Path }
function Write-File([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
# A residual folder: optional payload with a valid manifest, optional extra files.
function New-Residual([string]$Id, [switch]$Payload, [hashtable]$Files = @{}) {
    $app = Join-Path $root $Id
    New-Item -ItemType Directory -Path $app | Out-Null
    if ($Payload) {
        Write-File (Join-Path $app 'app.exe') "payload $Id"
        Write-File (Join-Path $app 'lib\core.dll') "core $Id"
        $manifest = @{ releaseId=$Id; sourceCommit=('b'*40); files=@(
            @{path='app.exe'; sha256=(Get-Sha256 (Join-Path $app 'app.exe'))},
            @{path='lib/core.dll'; sha256=(Get-Sha256 (Join-Path $app 'lib\core.dll'))}) }
        Write-File (Join-Path $app 'build-manifest.json') ($manifest | ConvertTo-Json -Depth 5)
        Write-File (Join-Path $app '.corevideo-prerelease-install') $Id
    }
    foreach ($relative in $Files.Keys) { Write-File (Join-Path $app $relative) $Files[$relative] }
    return $app
}
function New-Registered([string]$Id) {
    $app = New-Residual $Id -Payload
    $key = Join-Path $registryRoot "CoreVideoPro-$Id"
    New-Item -Path $key | Out-Null
    Set-ItemProperty -LiteralPath $key -Name InstallLocation -Value $app
    Set-ItemProperty -LiteralPath $key -Name Publisher -Value 'CoreVideo Pro'
    Set-ItemProperty -LiteralPath $key -Name DisplayName -Value "CoreVideo Pro $Id"
    return $app
}
function Get-Plan([string[]]$Running = @(), [string[]]$Protected = @()) {
    @(Get-ResidualInstallationPlan -CurrentDirectory $current -InstallRoot $root -RegistryRoot $registryRoot -RunningPaths $Running -ProtectedPaths $Protected)
}
function Find([object[]]$Plan, [string]$Dir) { return ,@($Plan | Where-Object { $_.directory -eq $Dir }) }
function Get-Inventory([string]$Dir) {
    # Files only (name, size, content hash), sorted: what a sweep could lose. Never follows reparse points.
    $lines = [Collections.Generic.List[string]]::new()
    $stack = [Collections.Generic.Stack[string]]::new(); $stack.Push($Dir)
    while ($stack.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $stack.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { $lines.Add("$($item.FullName)|reparse") }
            elseif ($item.PSIsContainer) { $lines.Add("$($item.FullName)|dir"); $stack.Push($item.FullName) }
            else { $lines.Add("$($item.FullName)|$($item.Length)|$(Get-Sha256 $item.FullName)") }
        }
    }
    @($lines | Sort-Object) -join "`n"
}

try {
    # --- Task 3: the shared metadata list -----------------------------------------------
    $names = @(Get-OwnedInstallMetadataNames)
    foreach ($n in 'Uninstall.exe','.camera-registration-before.json','.camera-desktop-registration-before.json','.corevideo-prerelease-install','build-manifest.json','previous-installations-cleanup.json') {
        Assert ($names -contains $n) "Shared metadata list lacks $n."
    }
    Assert ($names.Count -eq 6) "Shared metadata list has unexpected entries: $($names -join ', ')"

    $current = New-Registered 'beta-2026-10-23-current'
    $metaOnly = New-Residual 'beta-2026-10-01-metaonly' -Files @{ '.camera-desktop-registration-before.json' = '{}' }
    $recordings = New-Residual 'beta-2026-10-02-recordings' -Files @{ 'Recordings\CoreVideo Pro\take1.mp4' = 'operator video 1'; 'Recordings\CoreVideo Pro\take2.mp4' = 'operator video 2' }
    $modified = New-Residual 'beta-2026-10-03-modified' -Payload
    Write-File (Join-Path $modified 'lib\core.dll') 'operator patched this'
    $noManifest = New-Residual 'beta-2026-10-04-nomanifest' -Files @{ 'app.exe' = 'unproven'; 'previous-installations-cleanup.json' = '[]' }
    $registeredOld = New-Registered 'beta-2026-10-05-registered'
    $foreign = New-Residual 'NotAVersion' -Files @{ 'notes.txt' = 'not ours' }
    $running = New-Residual 'beta-2026-10-06-running' -Payload
    $linked = New-Residual 'beta-2026-10-07-linked' -Files @{ '.camera-desktop-registration-before.json' = '{}' }
    $junctionTarget = Join-Path $work 'junction-target'
    Write-File (Join-Path $junctionTarget 'precious.txt') 'outside the install root'
    New-Item -ItemType Junction -Path (Join-Path $linked 'redirect') -Target $junctionTarget | Out-Null
    $nestedMeta = New-Residual 'beta-2026-10-08-nestedmeta' -Files @{ 'Recordings\build-manifest.json' = 'operator file with a metadata name' }
    $camera = New-Residual 'beta-2026-10-09-camera' -Payload

    $before = @{}
    foreach ($d in Get-ChildItem -LiteralPath $root -Directory -Force) { $before[$d.FullName] = Get-Inventory $d.FullName }
    $targetBefore = Get-Inventory $junctionTarget

    $plan = Get-Plan -Running @((Join-Path $running 'app.exe')) -Protected @((Join-Path $camera 'corevideo-virtualcam.dll'))

    # (a) metadata-only folder
    $p = Find $plan $metaOnly
    Assert ($p.Count -eq 1 -and $p[0].status -eq 'ready') "(a) metadata-only folder not ready: $($p | ConvertTo-Json -Depth 4)"
    Assert ($p[0].metadata.Count -eq 1 -and $p[0].unknown.Count -eq 0 -and $p[0].payload.Count -eq 0) '(a) metadata-only folder misclassified.'
    # (b) recordings are unknown, never metadata
    $p = Find $plan $recordings
    Assert ($p[0].status -eq 'ready' -and $p[0].unknown.Count -eq 2 -and $p[0].metadata.Count -eq 0) '(b) recordings not classified unknown.'
    # (c) modified payload file is unknown, matching files are payload
    $p = Find $plan $modified
    Assert ($p[0].payload.Count -eq 1 -and $p[0].payload[0] -like '*\app.exe') '(c) proven payload not listed.'
    Assert ($p[0].unknown.Count -eq 1 -and $p[0].unknown[0] -like '*\lib\core.dll') '(c) modified payload not unknown.'
    Assert ($p[0].metadata.Count -eq 2) '(c) manifest + marker should be metadata.'
    # (d) no manifest: every non-metadata file is unknown
    $p = Find $plan $noManifest
    Assert ($p[0].unknown.Count -eq 1 -and $p[0].payload.Count -eq 0 -and $p[0].metadata.Count -eq 1) '(d) unproven file treated as payload.'
    # (e)(f)(g) registered, current and foreign-named folders are never candidates
    Assert ((Find $plan $registeredOld).Count -eq 0) '(e) registered folder returned.'
    Assert ((Find $plan $current).Count -eq 0) '(f) current folder returned.'
    Assert ((Find $plan $foreign).Count -eq 0) '(g) non-release folder returned.'
    # (h) running version skipped
    $p = Find $plan $running
    Assert ($p[0].status -eq 'skipped' -and $p[0].reason -match 'running') '(h) running version not skipped.'
    # (i) junction: retained, never entered
    $p = Find $plan $linked
    Assert ($p[0].status -eq 'retained' -and $p[0].reason -match 'eparse') "(i) junction not retained: $($p[0].reason)"
    Assert (@($p[0].unknown + $p[0].payload + $p[0].metadata | Where-Object { $_ -like '*precious*' }).Count -eq 0) '(i) junction was entered.'
    # (j) a metadata-named file below the root is unknown
    $p = Find $plan $nestedMeta
    Assert ($p[0].unknown.Count -eq 1 -and $p[0].metadata.Count -eq 0) '(j) nested metadata-named file not unknown.'
    # camera registration pointing into a folder: skipped
    $p = Find $plan $camera
    Assert ($p[0].status -eq 'skipped' -and $p[0].reason -match 'camera') '(camera) folder holding the registered camera not skipped.'
    # Classification wrote nothing.
    foreach ($d in $before.Keys) { Assert ((Get-Inventory $d) -eq $before[$d]) "Planning changed $d." }
    Assert ((Get-Inventory $junctionTarget) -eq $targetBefore) 'Planning changed the junction target.'

    # (k) the registered path uses the shared list: the desktop camera backup no longer strands a folder
    Write-File (Join-Path $registeredOld '.camera-desktop-registration-before.json') '{}'
    $registeredPlan = @(Get-PreviousInstallationPlan -CurrentDirectory $current -InstallRoot $root -RegistryRoot $registryRoot -RunningPaths @()) | Where-Object directory -eq $registeredOld
    $result = Remove-PreviousInstallation -Plan $registeredPlan
    Assert ($result.status -eq 'removed' -and !(Test-Path -LiteralPath $registeredOld)) "(k) registered retirement left the desktop camera backup: $($result.reason)"

    # --- Task 4: removal with recoverable backup (owner ruling D4) -------------------------
    $backupRoot = Join-Path $work 'LocalAppData\CoreVideoPro\backups'
    $backup = New-ResidualBackupFolder -BackupRoot $backupRoot
    Assert ((Split-Path -Leaf $backup) -match '^retired-install-remnants-\d{8}-\d{6}-[0-9a-f]{8}$') "Backup folder is not dated: $backup"
    function Retire([string]$Dir, [hashtable]$Extra = @{}) {
        $p = Find (Get-Plan -Running @((Join-Path $running 'app.exe')) -Protected @((Join-Path $camera 'corevideo-virtualcam.dll'))) $Dir
        Assert ($p.Count -eq 1) "No plan for $Dir"
        return Remove-ResidualInstallation -Plan $p[0] -BackupFolder $backup @Extra
    }
    function Read-Manifest { @((Get-Content -LiteralPath (Join-Path $backup 'backup-manifest.json') -Raw | ConvertFrom-Json).entries) }

    # (a) metadata-only: folder gone, no backup folder created
    $r = Retire $metaOnly
    Assert ($r.status -eq 'removed' -and !(Test-Path -LiteralPath $metaOnly)) "(a) metadata-only folder not removed: $($r.reason)"
    Assert (!(Test-Path -LiteralPath $backup)) '(a) backup folder created for a metadata-only folder.'

    # (b) recordings: moved byte-identical, manifest lists each, source gone
    $hash1 = Get-Sha256 (Join-Path $recordings 'Recordings\CoreVideo Pro\take1.mp4')
    $r = Retire $recordings
    Assert ($r.status -eq 'backed-up' -and $r.moved -eq 2 -and !(Test-Path -LiteralPath $recordings)) "(b) recordings folder not backed up: $($r.status) $($r.reason)"
    $dest1 = Join-Path $backup 'beta-2026-10-02-recordings\Recordings\CoreVideo Pro\take1.mp4'
    Assert ((Test-Path -LiteralPath $dest1) -and (Get-Sha256 $dest1) -eq $hash1) '(b) moved recording differs or is missing.'
    Assert ($r.backedUpTo -eq (Join-Path $backup 'beta-2026-10-02-recordings')) "(b) backedUpTo wrong: $($r.backedUpTo)"
    $m = Read-Manifest
    Assert (@($m | Where-Object { $_.releaseId -eq 'beta-2026-10-02-recordings' -and $_.status -eq 'moved' -and $_.sha256 -and $_.length -gt 0 }).Count -eq 2) '(b) manifest does not list both moves.'

    # modified payload: proven payload deleted, modified file backed up, folder gone
    $r = Retire $modified
    Assert ($r.status -eq 'backed-up' -and $r.moved -eq 1 -and !(Test-Path -LiteralPath $modified)) "(modified) not retired: $($r.reason)"
    Assert ((Get-Content -LiteralPath (Join-Path $backup 'beta-2026-10-03-modified\lib\core.dll') -Raw) -eq 'operator patched this') '(modified) modified payload not preserved.'

    # (c) -LeaveUnknown: nothing moved, folder kept, reason names the count
    $leave = New-Residual 'beta-2026-10-10-leave' -Files @{ 'Recordings\x.mp4' = 'keep me'; '.camera-desktop-registration-before.json' = '{}' }
    $inv = Get-Inventory $leave
    $r = Retire $leave @{ LeaveUnknown = $true }
    Assert ($r.status -eq 'retained' -and $r.reason -match 'operator content: 1 files') "(c) LeaveUnknown: $($r.status) $($r.reason)"
    Assert ((Get-Inventory $leave) -eq $inv) '(c) LeaveUnknown changed the folder.'

    # (d) destination collision: numeric suffix, the existing file is untouched
    $collide = New-Residual 'beta-2026-10-11-collide' -Files @{ 'Recordings\x.mp4' = 'second' }
    Write-File (Join-Path $backup 'beta-2026-10-11-collide\Recordings\x.mp4') 'already here'
    $r = Retire $collide
    Assert ($r.status -eq 'backed-up') "(d) collision folder: $($r.reason)"
    Assert ((Get-Content -LiteralPath (Join-Path $backup 'beta-2026-10-11-collide\Recordings\x.mp4') -Raw) -eq 'already here') '(d) existing backup file overwritten.'
    Assert ((Get-Content -LiteralPath (Join-Path $backup 'beta-2026-10-11-collide\Recordings\x (1).mp4') -Raw) -eq 'second') '(d) collision not suffixed.'

    # (e) injected crash after one move: every file is at its source or at a listed destination
    $crash = New-Residual 'beta-2026-10-12-crash' -Files @{ 'Recordings\a.mp4' = 'a'; 'Recordings\b.mp4' = 'b'; '.camera-desktop-registration-before.json' = '{}' }
    $r = Retire $crash @{ FailAfterMoves = 1 }
    Assert ($r.status -eq 'retained' -and $r.moved -eq 1 -and (Test-Path -LiteralPath $crash)) "(e) injected failure not retained: $($r.status) $($r.reason)"
    $m = @(Read-Manifest | Where-Object releaseId -eq 'beta-2026-10-12-crash')
    Assert ($m.Count -eq 2) '(e) manifest does not list every planned move.'
    foreach ($e in $m) { Assert ((Test-Path -LiteralPath $e.source) -xor (Test-Path -LiteralPath $e.destination)) "(e) $($e.source) is neither at source nor destination (or both)." }
    Assert (Test-Path -LiteralPath (Join-Path $crash '.camera-desktop-registration-before.json')) '(e) metadata deleted after a failed move.'
    #     a locked operator file keeps the folder
    $locked = New-Residual 'beta-2026-10-13-locked' -Files @{ 'Recordings\l.mp4' = 'locked' }
    $lock = [IO.File]::Open((Join-Path $locked 'Recordings\l.mp4'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { $r = Retire $locked } finally { $lock.Dispose() }
    Assert ($r.status -eq 'retained' -and (Test-Path -LiteralPath (Join-Path $locked 'Recordings\l.mp4'))) "(e) locked file: $($r.status) $($r.reason)"

    # (f) backup on another volume: retained, nothing moved
    $vol = New-Residual 'beta-2026-10-14-volume' -Files @{ 'Recordings\v.mp4' = 'v' }
    $inv = Get-Inventory $vol
    $r = Retire $vol @{ BackupVolumeRoot = 'Z:\' }
    Assert ($r.status -eq 'retained' -and $r.reason -match 'another volume' -and (Get-Inventory $vol) -eq $inv) "(f) cross-volume backup: $($r.status) $($r.reason)"

    # (g) running version and (h) junction never reach removal; camera folder neither
    foreach ($d in $running, $linked, $camera) {
        $inv = Get-Inventory $d
        $r = Retire $d
        Assert ($r.status -in 'skipped','retained' -and (Get-Inventory $d) -eq $inv) "(g/h) $d was changed: $($r.status)"
    }
    Assert ((Get-Inventory $junctionTarget) -eq $targetBefore) '(h) junction target changed.'
    Assert (Test-Path -LiteralPath (Join-Path $foreign 'notes.txt')) 'Foreign folder changed.'
    Assert (Test-Path -LiteralPath (Join-Path $current 'app.exe')) 'Current install changed.'

    [ordered]@{ passed=$true; checks=$checks; evidence=$work } | ConvertTo-Json
} finally {
    $resolvedKey = Get-Item -LiteralPath $registryRoot
    if ($resolvedKey.PSChildName -ne $caseId) { throw 'Unexpected test registry cleanup target.' }
    Remove-Item -LiteralPath $registryRoot -Recurse
}
