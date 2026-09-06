param([switch]$ValidateOnly)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskProject = Join-Path $taskRepo 'native-shell\CoreVideoPro.WinUI\CoreVideoPro.WinUI.csproj'
[xml]$taskProjectXml = Get-Content -LiteralPath $taskProject -Raw
$taskRuntimeReference = @($taskProjectXml.Project.ItemGroup.PackageReference) |
    Where-Object { $_.Include -eq 'Microsoft.WindowsAppSDK.Runtime' }
if (@($taskRuntimeReference).Count -ne 1 -or -not $taskRuntimeReference.Version) {
    throw 'Expected one pinned Microsoft.WindowsAppSDK.Runtime reference.'
}
$taskPackages = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else {
    Join-Path $env:USERPROFILE '.nuget\packages'
}
$taskRuntime = Join-Path $taskPackages "microsoft.windowsappsdk.runtime\$($taskRuntimeReference.Version)\tools\MSIX\win10-x64"
# Register the exact restored runtime, not an independently downloaded installer.
# The remaining packages depend on the framework, so its registration comes first.
$taskNames = @('Microsoft.WindowsAppRuntime.2.msix',
    'Microsoft.WindowsAppRuntime.DDLM.2.msix',
    'Microsoft.WindowsAppRuntime.Main.2.msix',
    'Microsoft.WindowsAppRuntime.Singleton.2.msix')
$taskPaths = @($taskNames | ForEach-Object { Join-Path $taskRuntime $_ })
foreach ($taskPath in $taskPaths) {
    if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) {
        throw "Missing restored WinUI test runtime: $taskPath. Run dotnet restore first."
    }
}
if ($ValidateOnly) {
    Write-Host "Validated four runtime packages from Microsoft.WindowsAppSDK.Runtime $($taskRuntimeReference.Version)."
    exit 0
}
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'Runtime registration is restricted to the ephemeral GitHub Actions runner; use -ValidateOnly locally.'
}
foreach ($taskPath in $taskPaths) {
    Write-Host "Registering test runtime: $(Split-Path -Leaf $taskPath)"
    Add-AppxPackage -Path $taskPath -ErrorAction Stop
}
