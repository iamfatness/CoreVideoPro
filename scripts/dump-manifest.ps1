param([string]$Binary)
$mt = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter mt.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $mt) { throw "mt.exe not found" }
$out = Join-Path $env:TEMP "corevideo-manifest.xml"
if (Test-Path $out) { Remove-Item $out -Force }
& $mt.FullName -nologo -inputresource:"$Binary;#1" -out:$out
Get-Content $out