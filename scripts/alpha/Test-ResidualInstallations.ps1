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

    [ordered]@{ passed=$true; checks=$checks; evidence=$work } | ConvertTo-Json
} finally {
    $resolvedKey = Get-Item -LiteralPath $registryRoot
    if ($resolvedKey.PSChildName -ne $caseId) { throw 'Unexpected test registry cleanup target.' }
    Remove-Item -LiteralPath $registryRoot -Recurse
}
