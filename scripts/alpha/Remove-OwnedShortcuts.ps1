[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$AppDirectory)
$ErrorActionPreference = 'Stop'
$app = [IO.Path]::GetFullPath($AppDirectory).TrimEnd('\')
$expected = Join-Path $app 'StartCoreVideo.cmd'
$shell = New-Object -ComObject WScript.Shell
try {
    foreach ($folder in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
        $path = Join-Path $folder 'CoreVideo Pro.lnk'
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $shortcut = $shell.CreateShortcut($path)
            try {
                if ([string]::Equals($shortcut.TargetPath, $expected, [StringComparison]::OrdinalIgnoreCase)) {
                    Remove-Item -LiteralPath $path
                }
            } finally { [Runtime.InteropServices.Marshal]::ReleaseComObject($shortcut) | Out-Null }
        }
    }
} finally { [Runtime.InteropServices.Marshal]::ReleaseComObject($shell) | Out-Null }
