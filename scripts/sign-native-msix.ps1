# Optional local-dev signing helper for artifacts/native/CoreVideoPro.msix.
# Creates or reuses a self-signed Authenticode cert when SignTool is available.
# When signing tools are missing, prints setup steps and exits 0 (document-only stub).
param(
  [string]$PackagePath = "",
  [string]$CertSubject = "CN=CoreVideo Pro Dev",
  [string]$CertStore = "Cert:\CurrentUser\My"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $PackagePath) {
  $PackagePath = Join-Path $repoRoot "artifacts\native\CoreVideoPro.msix"
}

function Find-SignTool {
  $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
  if (-not (Test-Path $kits)) {
    return $null
  }
  $latest = Get-ChildItem -Path $kits -Directory -Filter "10.*" |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
  if (-not $latest) {
    return $null
  }
  $tool = Join-Path $latest.FullName "x64\signtool.exe"
  if (Test-Path $tool) { return $tool }
  return $null
}

function Write-SigningDocs {
  Write-Host ""
  Write-Host "[sign:native-msix] Signing tools not available; document-only mode." -ForegroundColor Yellow
  Write-Host "  1. Install Windows SDK (SignTool) or Visual Studio with Windows SDK components." -ForegroundColor DarkGray
  Write-Host "  2. Create a dev cert:" -ForegroundColor DarkGray
  Write-Host "     New-SelfSignedCertificate -Type Custom -Subject `"$CertSubject`" -KeyUsage DigitalSignature -FriendlyName `"CoreVideo Pro Dev`" -CertStoreLocation `"$CertStore`" -TextExtension @(`"2.5.29.37={text}1.3.6.1.5.5.7.3.3`")" -ForegroundColor DarkGray
  Write-Host "  3. Trust it: certmgr.msc -> Trusted People -> import the .cer export." -ForegroundColor DarkGray
  Write-Host "  4. Sign: signtool sign /fd SHA256 /a /n `"CoreVideo Pro Dev`" `"$PackagePath`"" -ForegroundColor DarkGray
  Write-Host "  Unsigned install still works: Add-AppxPackage -Path `"$PackagePath`" -AllowUnsigned" -ForegroundColor DarkGray
}

if (-not (Test-Path $PackagePath)) {
  Write-Host "[sign:native-msix] MSIX not found at $PackagePath; run npm run pack:native:msix first." -ForegroundColor Yellow
  exit 0
}

$signTool = Find-SignTool
if (-not $signTool) {
  Write-SigningDocs
  exit 0
}

$cert = Get-ChildItem -Path $CertStore -CodeSigningCert -ErrorAction SilentlyContinue |
  Where-Object { $_.Subject -eq $CertSubject } |
  Sort-Object NotAfter -Descending |
  Select-Object -First 1

if (-not $cert) {
  Write-Host "[sign:native-msix] creating self-signed dev certificate ($CertSubject)..." -ForegroundColor Cyan
  $cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject $CertSubject `
    -KeyUsage DigitalSignature `
    -FriendlyName "CoreVideo Pro Dev" `
    -CertStoreLocation $CertStore `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
}

Write-Host "[sign:native-msix] signing $PackagePath with thumbprint $($cert.Thumbprint)..." -ForegroundColor Cyan
& $signTool sign /fd SHA256 /sha1 $cert.Thumbprint $PackagePath
if ($LASTEXITCODE -ne 0) {
  Write-Host "[sign:native-msix] SignTool failed; see document-only steps below." -ForegroundColor Yellow
  Write-SigningDocs
  exit 0
}

Write-Host ""
Write-Host "[sign:native-msix] signed package ready:" -ForegroundColor Green
Write-Host "  $PackagePath"
Write-Host "  Trust the cert under Local Computer -> Trusted People if installs are blocked." -ForegroundColor DarkGray