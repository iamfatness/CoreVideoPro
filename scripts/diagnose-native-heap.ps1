param(
  [string]$Configuration = "RelWithDebInfo",
  [switch]$AllowWhileObs
)

$ErrorActionPreference = "Stop"

if (-not $AllowWhileObs -and (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
  throw "OBS is running. Native heap instrumentation is intentionally blocked during a live soak. Stop OBS or pass -AllowWhileObs explicitly."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$nativeRoot = Join-Path $repoRoot "native"
$buildRoot = Join-Path $nativeRoot "build-asan"
$artifactRoot = Join-Path $repoRoot "artifacts\diagnostics\native-heap"
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

$currentProcess = Get-Process -Id $PID
$currentProcess.PriorityClass = "BelowNormal"

cmake -S $nativeRoot -B $buildRoot -A x64 `
  '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /fsanitize=address /Zi' `
  '-DCMAKE_EXE_LINKER_FLAGS=/INCREMENTAL:NO'
if ($LASTEXITCODE -ne 0) {
  throw "ASan CMake configure failed with exit code $LASTEXITCODE."
}

cmake --build $buildRoot --config $Configuration --target corevideo-native-tests -- /m:1 /p:BuildInParallel=false
if ($LASTEXITCODE -ne 0) {
  throw "ASan native test build failed with exit code $LASTEXITCODE."
}

$testExecutable = Join-Path $buildRoot "corevideo-native-tests.exe"
if (-not (Test-Path -LiteralPath $testExecutable)) {
  throw "ASan test executable was not produced: $testExecutable"
}

$visualStudioRoot = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio"
$asanRuntime = Get-ChildItem -Path $visualStudioRoot -Recurse `
  -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match "\\bin\\Hostx64\\x64\\" } |
  Sort-Object FullName -Descending |
  Select-Object -First 1
if (-not $asanRuntime) {
  throw "The MSVC x64 AddressSanitizer runtime could not be found under $visualStudioRoot."
}
$env:PATH = "$($asanRuntime.DirectoryName);$env:PATH"
$env:ASAN_OPTIONS = "halt_on_error=1:abort_on_error=1:windows_hook_rtl_allocators=true"
$filters = @("AudioDsp.*", "MediaCoreCommand.*")
foreach ($filter in $filters) {
  $safeName = $filter.Replace("*", "all").Replace(".", "-")
  $logPath = Join-Path $artifactRoot "$safeName.log"
  & $testExecutable "--gtest_filter=$filter" 2>&1 | Tee-Object -FilePath $logPath
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "Heap diagnostic filter $filter failed with exit code $LASTEXITCODE. Evidence: $logPath"
  }
}

Write-Host "Native heap evidence written to $artifactRoot"
