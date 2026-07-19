# Pester-free assertions for scripts/sign-native-msix.ps1 (beta spec D2).
# Exercises the -Mode production decision logic via -DryRun (no real signing)
# plus the preserved dev-mode exit-0 behavior. Exits non-zero on any
# unexpected result.
#
# What it does on this machine:
#   - creates a scratch dir with a dummy package + dummy signtool/dlib/metadata
#   - creates TWO throwaway self-signed certs in Cert:\CurrentUser\My
#     ("CN=CoreVideo Pro Dev" and a mismatching subject), exports both to PFX,
#     and REMOVES them from the store in the finally block
#   - spawns child powershell -NoProfile runs of the script per case
param()

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$targetScript = Join-Path $repoRoot "scripts\sign-native-msix.ps1"
if (-not (Test-Path $targetScript)) {
  Write-Host "FATAL: cannot find $targetScript" -ForegroundColor Red
  exit 1
}

$signEnvNames = @(
  "COREVIDEO_SIGNTOOL_PATH",
  "COREVIDEO_SIGN_DLIB",
  "COREVIDEO_SIGN_METADATA",
  "COREVIDEO_SIGN_PFX_PATH",
  "COREVIDEO_SIGN_PFX_PASSWORD",
  "COREVIDEO_SIGN_CERT_THUMBPRINT",
  "COREVIDEO_SIGN_EXPECTED_PUBLISHER",
  "COREVIDEO_SIGN_TIMESTAMP_URL",
  "COREVIDEO_MSIX_PACKAGE_PATH",
  "COREVIDEO_SIGN_CERT_SUBJECT",
  "COREVIDEO_SIGN_CERT_STORE"
)

$script:failCount = 0
$script:caseCount = 0

function Invoke-SignCase {
  param(
    [string]$Name,
    [hashtable]$EnvVars,
    [string[]]$ScriptArgs,
    [int]$ExpectedExit,
    [string[]]$ExpectPatterns = @(),
    [string[]]$ForbidPatterns = @()
  )
  $script:caseCount++
  foreach ($n in $signEnvNames) {
    if (Test-Path "Env:$n") { Remove-Item "Env:$n" }
  }
  foreach ($k in $EnvVars.Keys) {
    Set-Item -Path "Env:$k" -Value $EnvVars[$k]
  }

  $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $targetScript @ScriptArgs | Out-String
  $exitCode = $LASTEXITCODE

  foreach ($n in $signEnvNames) {
    if (Test-Path "Env:$n") { Remove-Item "Env:$n" }
  }

  $problems = @()
  if ($exitCode -ne $ExpectedExit) {
    $problems += "expected exit $ExpectedExit, got $exitCode"
  }
  foreach ($p in $ExpectPatterns) {
    if ($output -notmatch $p) { $problems += "output missing expected pattern '$p'" }
  }
  foreach ($p in $ForbidPatterns) {
    if ($output -match $p) { $problems += "output contains forbidden pattern '$p'" }
  }

  if ($problems.Count -eq 0) {
    Write-Host ("PASS  {0}" -f $Name) -ForegroundColor Green
  } else {
    $script:failCount++
    Write-Host ("FAIL  {0}" -f $Name) -ForegroundColor Red
    foreach ($p in $problems) { Write-Host ("      - {0}" -f $p) -ForegroundColor Red }
    Write-Host "      ---- captured output ----" -ForegroundColor DarkGray
    foreach ($line in ($output -split "`r?`n")) { Write-Host ("      | {0}" -f $line) -ForegroundColor DarkGray }
  }
}

# --- Scratch fixtures ------------------------------------------------------
$work = Join-Path $env:TEMP ("d2-sign-test-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null

$dummyPackage = Join-Path $work "CoreVideoPro.msix"
Set-Content -Path $dummyPackage -Value "not a real msix" -Encoding ascii
$fakeSignTool = Join-Path $work "signtool.exe"
Set-Content -Path $fakeSignTool -Value "fake" -Encoding ascii
$missingSignTool = Join-Path $work "no-such-dir\signtool.exe"
$dummyDlib = Join-Path $work "Azure.CodeSigning.Dlib.dll"
Set-Content -Path $dummyDlib -Value "fake" -Encoding ascii
$dummyMetadata = Join-Path $work "trusted-signing-metadata.json"
Set-Content -Path $dummyMetadata -Value '{"Endpoint":"https://eus.codesigning.azure.net/","CodeSigningAccountName":"x","CertificateProfileName":"y"}' -Encoding ascii

$goodPfx = Join-Path $work "good.pfx"
$badPfx = Join-Path $work "mismatch.pfx"
$pfxPasswordPlain = "d2-test-password"
$pfxPassword = ConvertTo-SecureString -String $pfxPasswordPlain -AsPlainText -Force

# Manifest publisher today (see native-shell/CoreVideoPro.WinUI/Package.appxmanifest).
$manifestPublisher = "CN=CoreVideo Pro Dev"

$goodCert = $null
$badCert = $null
try {
  $goodCert = New-SelfSignedCertificate -Type Custom -Subject $manifestPublisher `
    -KeyUsage DigitalSignature -FriendlyName "CoreVideo D2 signing TEST cert (delete me)" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
  Export-PfxCertificate -Cert $goodCert -FilePath $goodPfx -Password $pfxPassword | Out-Null

  $badCert = New-SelfSignedCertificate -Type Custom -Subject "CN=Not The Manifest Publisher" `
    -KeyUsage DigitalSignature -FriendlyName "CoreVideo D2 signing TEST cert (delete me)" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
  Export-PfxCertificate -Cert $badCert -FilePath $badPfx -Password $pfxPassword | Out-Null

  # --- Cases ---------------------------------------------------------------

  # 1. Production, signtool missing -> hard fail (exit 2), even with a cert route configured.
  Invoke-SignCase -Name "production: missing signtool hard-fails (exit 2)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $missingSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_PFX_PATH = $goodPfx
      COREVIDEO_SIGN_PFX_PASSWORD = $pfxPasswordPlain
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 2 `
    -ExpectPatterns @("signtool", "ERROR")

  # 2. Production, no cert configuration -> hard fail (exit 3).
  Invoke-SignCase -Name "production: no cert env hard-fails (exit 3)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 3 `
    -ExpectPatterns @("no production signing configuration")

  # 3. Production, BOTH trusted-signing and PFX routes configured -> ambiguous (exit 3).
  Invoke-SignCase -Name "production: ambiguous routes hard-fail (exit 3)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_DLIB = $dummyDlib
      COREVIDEO_SIGN_METADATA = $dummyMetadata
      COREVIDEO_SIGN_PFX_PATH = $goodPfx
      COREVIDEO_SIGN_PFX_PASSWORD = $pfxPasswordPlain
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 3 `
    -ExpectPatterns @("ambiguous")

  # 4. Production dry run, PFX route selected; publisher matches the manifest -> plan resolves (exit 0).
  Invoke-SignCase -Name "production: PFX route selected + publisher OK (dry run exit 0)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_PFX_PATH = $goodPfx
      COREVIDEO_SIGN_PFX_PASSWORD = $pfxPasswordPlain
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 0 `
    -ExpectPatterns @("Route:\s+pfx", "Publisher check:\s+OK", "DRY RUN") `
    -ForbidPatterns @([regex]::Escape($pfxPasswordPlain))

  # 5. Production, PFX whose subject does NOT match the manifest Publisher -> exit 4.
  Invoke-SignCase -Name "production: publisher mismatch hard-fails (exit 4)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_PFX_PATH = $badPfx
      COREVIDEO_SIGN_PFX_PASSWORD = $pfxPasswordPlain
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 4 `
    -ExpectPatterns @("Publisher mismatch", "Package\.appxmanifest")

  # 6. Production, thumbprint route (cert still in CurrentUser\My) -> exit 0.
  Invoke-SignCase -Name "production: thumbprint route selected (dry run exit 0)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_CERT_THUMBPRINT = $goodCert.Thumbprint
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 0 `
    -ExpectPatterns @("Route:\s+thumbprint", "Publisher check:\s+OK", "DRY RUN")

  # 7. Production, Trusted Signing route with expected publisher set -> exit 0, check OK.
  Invoke-SignCase -Name "production: trusted-signing route + expected publisher (dry run exit 0)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_DLIB = $dummyDlib
      COREVIDEO_SIGN_METADATA = $dummyMetadata
      COREVIDEO_SIGN_EXPECTED_PUBLISHER = $manifestPublisher
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 0 `
    -ExpectPatterns @("Route:\s+trusted-signing", "Publisher check:\s+OK", "DRY RUN")

  # 8. Production, Trusted Signing route WITHOUT expected publisher -> loud SKIPPED warning, still resolves.
  Invoke-SignCase -Name "production: trusted-signing without expected publisher warns loudly" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $fakeSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
      COREVIDEO_SIGN_DLIB = $dummyDlib
      COREVIDEO_SIGN_METADATA = $dummyMetadata
    } `
    -ScriptArgs @("-Mode", "production", "-DryRun") `
    -ExpectedExit 0 `
    -ExpectPatterns @("WARNING", "SKIPPED", "Publisher check:\s+SKIPPED")

  # 9. Dev mode, signtool missing -> preserved exit 0 but with a LOUD unsigned warning.
  Invoke-SignCase -Name "dev: missing signtool still exits 0 with LOUD warning" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $missingSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
    } `
    -ScriptArgs @("-Mode", "dev") `
    -ExpectedExit 0 `
    -ExpectPatterns @("WARNING", "UNSIGNED")

  # 10. Default mode is dev (no -Mode arg) -> same preserved behavior.
  Invoke-SignCase -Name "default mode is dev (exit 0 without signtool)" `
    -EnvVars @{
      COREVIDEO_SIGNTOOL_PATH = $missingSignTool
      COREVIDEO_MSIX_PACKAGE_PATH = $dummyPackage
    } `
    -ScriptArgs @() `
    -ExpectedExit 0 `
    -ExpectPatterns @("WARNING", "UNSIGNED")
}
finally {
  if ($goodCert) {
    try { Remove-Item ("Cert:\CurrentUser\My\" + $goodCert.Thumbprint) -ErrorAction Stop } catch {}
  }
  if ($badCert) {
    try { Remove-Item ("Cert:\CurrentUser\My\" + $badCert.Thumbprint) -ErrorAction Stop } catch {}
  }
  try { Remove-Item -Recurse -Force $work -ErrorAction Stop } catch {}
}

Write-Host ""
if ($script:failCount -gt 0) {
  Write-Host ("[test-sign-native-msix] FAIL: {0}/{1} cases failed" -f $script:failCount, $script:caseCount) -ForegroundColor Red
  exit 1
}
Write-Host ("[test-sign-native-msix] PASS: all {0} cases" -f $script:caseCount) -ForegroundColor Green
exit 0
