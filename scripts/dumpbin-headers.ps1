param([string]$Binary)
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } |
    Select-Object -First 1
if (-not $dumpbin) { throw "dumpbin not found" }
& $dumpbin.FullName /HEADERS $Binary | Select-String -Pattern "subsystem|magic|machine"