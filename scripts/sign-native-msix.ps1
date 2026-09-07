# Signing helper for artifacts/native/CoreVideoPro.msix (beta spec D2).
#
# Modes:
#   -Mode dev (default)  Self-signed local dev cert. Tolerates missing SignTool
#                        (exits 0 with a LOUD warning) -- dev convenience only.
#   -Mode production     Hard-fails (non-zero exit, loud message) on ANY gap:
#                        missing signtool, missing/ambiguous cert config, sign
#                        failure, timestamp failure, or verify failure. A
#                        production pipeline must NEVER silently emit an
#                        unsigned artifact (spec section 7 invariants).
#   -DryRun              Resolve toolchain + cert route + publisher check and
#                        print the signing plan WITHOUT invoking signtool
#                        sign/verify. All decision-logic failures still fail.
#
# Shared environment:
#   COREVIDEO_MSIX_PACKAGE_PATH   MSIX to sign (default: artifacts/native/CoreVideoPro.msix)
#   COREVIDEO_SIGNTOOL_PATH       Explicit signtool.exe path. When set it is
#                                 authoritative: if the file does not exist,
#                                 signtool counts as NOT FOUND (no kit probing).
#                                 When unset, the newest Windows Kits 10 x64
#                                 signtool is used.
#
# Dev-mode environment (unchanged behavior):
#   COREVIDEO_SIGN_CERT_SUBJECT   Authenticode subject (default: CN=CoreVideo Pro Dev)
#   COREVIDEO_SIGN_CERT_STORE     Cert store path (default: Cert:\CurrentUser\My)
#
# Production-mode environment -- configure exactly ONE signing route:
#   Route "trusted-signing" (Azure Trusted Signing via signtool /dlib):
#     COREVIDEO_SIGN_DLIB               Path to Azure.CodeSigning.Dlib.dll
#                                       (x64, from the Trusted Signing Client tools)
#     COREVIDEO_SIGN_METADATA           Path to the JSON metadata file
#                                       ({"Endpoint": "...", "CodeSigningAccountName": "...",
#                                         "CertificateProfileName": "..."})
#     COREVIDEO_SIGN_EXPECTED_PUBLISHER Expected cert subject (e.g. "CN=..., O=..., C=...")
#                                       compared against the AppxManifest Publisher.
#                                       If unset the publisher check is SKIPPED with a
#                                       loud warning (the dlib does not expose the cert
#                                       subject before signing).
#     (Azure auth -- AZURE_TENANT_ID / AZURE_CLIENT_ID / AZURE_CLIENT_SECRET or
#     workload identity -- is consumed by the dlib itself, not this script.)
#   Route "pfx" (classic cert file):
#     COREVIDEO_SIGN_PFX_PATH           Path to the .pfx
#     COREVIDEO_SIGN_PFX_PASSWORD       PFX password (omit only for passwordless PFX)
#   Route "thumbprint" (machine/user cert store):
#     COREVIDEO_SIGN_CERT_THUMBPRINT    SHA1 thumbprint of a cert in
#                                       Cert:\CurrentUser\My or Cert:\LocalMachine\My
#   Timestamping (REQUIRED in production; RFC3161 via /tr + /td SHA256):
#     COREVIDEO_SIGN_TIMESTAMP_URL      Default: http://timestamp.digicert.com
#                                       A timestamp failure fails the sign step.
#
# Publisher match: MSIX install fails when the AppxManifest Publisher does not
# equal the signing cert subject. Production mode reads the Publisher from the
# MSIX itself (falling back to native-shell/CoreVideoPro.WinUI/Package.appxmanifest)
# and hard-fails on mismatch -- update the manifest Publisher (today it is
# "CN=CoreVideo Pro Dev") to the real cert subject before a production sign.
#
# Production exit codes:
#   0 ok | 2 signtool missing | 3 cert config missing/ambiguous/invalid
#   4 publisher mismatch | 5 signing failed | 6 verification failed
#   7 package missing
param(
  [ValidateSet("dev", "production")]
  [string]$Mode = "dev",
  [string]$PackagePath = $(if ($env:COREVIDEO_MSIX_PACKAGE_PATH) { $env:COREVIDEO_MSIX_PACKAGE_PATH } else { "" }),
  [string]$CertSubject = $(if ($env:COREVIDEO_SIGN_CERT_SUBJECT) { $env:COREVIDEO_SIGN_CERT_SUBJECT } else { "CN=CoreVideo Pro Dev" }),
  [string]$CertStore = $(if ($env:COREVIDEO_SIGN_CERT_STORE) { $env:COREVIDEO_SIGN_CERT_STORE } else { "Cert:\CurrentUser\My" }),
  [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $PackagePath) {
  $PackagePath = Join-Path $repoRoot "artifacts\native\CoreVideoPro.msix"
}

function Fail {
  param([int]$Code, [string]$Message)
  Write-Host ""
  Write-Host "[sign:native-msix] ERROR: $Message" -ForegroundColor Red
  Write-Host "[sign:native-msix] FAILED (exit $Code). No unsigned artifact will be silently accepted in production mode." -ForegroundColor Red
  exit $Code
}

function Find-SignTool {
  if ($env:COREVIDEO_SIGNTOOL_PATH) {
    # Explicit override is authoritative: missing file = signtool NOT FOUND.
    if (Test-Path $env:COREVIDEO_SIGNTOOL_PATH) { return $env:COREVIDEO_SIGNTOOL_PATH }
    return $null
  }
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

function Get-NormalizedSubject {
  param([string]$Subject)
  if ($null -eq $Subject) { return "" }
  # Tolerate "CN=X, O=Y" vs "CN=X,O=Y" spacing differences only.
  return ($Subject.Trim() -replace ",\s+", ",")
}

function Get-ManifestPublisher {
  # Prefer the AppxManifest inside the MSIX (what installs will actually check);
  # fall back to the repo manifest (dry runs before the package is built).
  param([string]$MsixPath)
  if (Test-Path $MsixPath) {
    try {
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $zip = [System.IO.Compression.ZipFile]::OpenRead($MsixPath)
      try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq "AppxManifest.xml" } | Select-Object -First 1
        if ($entry) {
          $reader = New-Object System.IO.StreamReader($entry.Open())
          try { $manifestText = $reader.ReadToEnd() } finally { $reader.Dispose() }
          [xml]$manifestXml = $manifestText
          $publisher = $manifestXml.Package.Identity.Publisher
          if ($publisher) {
            return @{ Publisher = $publisher; Source = "AppxManifest.xml inside $MsixPath" }
          }
        }
      } finally {
        $zip.Dispose()
      }
    } catch {
      Write-Host "[sign:native-msix] note: could not read AppxManifest from the MSIX ($($_.Exception.Message)); falling back to the repo manifest." -ForegroundColor DarkGray
    }
  }
  $repoManifest = Join-Path $repoRoot "native-shell\CoreVideoPro.WinUI\Package.appxmanifest"
  if (Test-Path $repoManifest) {
    [xml]$manifestXml = Get-Content -Path $repoManifest -Raw
    $publisher = $manifestXml.Package.Identity.Publisher
    if ($publisher) {
      return @{ Publisher = $publisher; Source = $repoManifest }
    }
  }
  return $null
}

function Resolve-ProductionRoute {
  # Auto-picks the signing route from which env vars are present.
  # Returns a hashtable; calls Fail (exit 3) on none/ambiguous/invalid config.
  $tsConfigured = [bool]($env:COREVIDEO_SIGN_DLIB -or $env:COREVIDEO_SIGN_METADATA)
  $pfxConfigured = [bool]$env:COREVIDEO_SIGN_PFX_PATH
  $tpConfigured = [bool]$env:COREVIDEO_SIGN_CERT_THUMBPRINT

  $configuredCount = 0
  if ($tsConfigured) { $configuredCount++ }
  if ($pfxConfigured) { $configuredCount++ }
  if ($tpConfigured) { $configuredCount++ }

  if ($configuredCount -eq 0) {
    Fail 3 ("no production signing configuration found. Set exactly one route: " +
      "COREVIDEO_SIGN_DLIB + COREVIDEO_SIGN_METADATA (Azure Trusted Signing), " +
      "COREVIDEO_SIGN_PFX_PATH [+ COREVIDEO_SIGN_PFX_PASSWORD] (PFX file), or " +
      "COREVIDEO_SIGN_CERT_THUMBPRINT (cert store).")
  }
  if ($configuredCount -gt 1) {
    $present = @()
    if ($tsConfigured) { $present += "trusted-signing (COREVIDEO_SIGN_DLIB/COREVIDEO_SIGN_METADATA)" }
    if ($pfxConfigured) { $present += "pfx (COREVIDEO_SIGN_PFX_PATH)" }
    if ($tpConfigured) { $present += "thumbprint (COREVIDEO_SIGN_CERT_THUMBPRINT)" }
    Fail 3 ("ambiguous signing configuration -- multiple routes present: " + ($present -join "; ") +
      ". Unset all but one so the pipeline is deterministic.")
  }

  if ($tsConfigured) {
    if (-not $env:COREVIDEO_SIGN_DLIB) {
      Fail 3 "Trusted Signing route requires COREVIDEO_SIGN_DLIB (Azure.CodeSigning.Dlib.dll path); only COREVIDEO_SIGN_METADATA is set."
    }
    if (-not $env:COREVIDEO_SIGN_METADATA) {
      Fail 3 "Trusted Signing route requires COREVIDEO_SIGN_METADATA (JSON metadata file path); only COREVIDEO_SIGN_DLIB is set."
    }
    if (-not (Test-Path $env:COREVIDEO_SIGN_DLIB)) {
      Fail 3 "COREVIDEO_SIGN_DLIB points at a missing file: $($env:COREVIDEO_SIGN_DLIB)"
    }
    if (-not (Test-Path $env:COREVIDEO_SIGN_METADATA)) {
      Fail 3 "COREVIDEO_SIGN_METADATA points at a missing file: $($env:COREVIDEO_SIGN_METADATA)"
    }
    return @{
      Route = "trusted-signing"
      Dlib = $env:COREVIDEO_SIGN_DLIB
      Metadata = $env:COREVIDEO_SIGN_METADATA
      ExpectedPublisher = $env:COREVIDEO_SIGN_EXPECTED_PUBLISHER
    }
  }

  if ($pfxConfigured) {
    if (-not (Test-Path $env:COREVIDEO_SIGN_PFX_PATH)) {
      Fail 3 "COREVIDEO_SIGN_PFX_PATH points at a missing file: $($env:COREVIDEO_SIGN_PFX_PATH)"
    }
    if (-not $env:COREVIDEO_SIGN_PFX_PASSWORD) {
      Write-Host "[sign:native-msix] WARNING: COREVIDEO_SIGN_PFX_PASSWORD is not set -- assuming a passwordless PFX." -ForegroundColor Yellow
    }
    $cert = $null
    try {
      $flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::DefaultKeySet
      $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
        $env:COREVIDEO_SIGN_PFX_PATH, [string]$env:COREVIDEO_SIGN_PFX_PASSWORD, $flags)
    } catch {
      Fail 3 "could not load the PFX at $($env:COREVIDEO_SIGN_PFX_PATH) (bad password or corrupt file): $($_.Exception.Message)"
    }
    return @{
      Route = "pfx"
      PfxPath = $env:COREVIDEO_SIGN_PFX_PATH
      HasPassword = [bool]$env:COREVIDEO_SIGN_PFX_PASSWORD
      CertSubject = $cert.Subject
      CertThumbprint = $cert.Thumbprint
    }
  }

  # Thumbprint route.
  $thumbprint = ($env:COREVIDEO_SIGN_CERT_THUMBPRINT -replace "\s", "").ToUpperInvariant()
  $storeCert = $null
  $storeName = $null
  foreach ($candidateStore in @("Cert:\CurrentUser\My", "Cert:\LocalMachine\My")) {
    $found = Get-ChildItem -Path $candidateStore -ErrorAction SilentlyContinue |
      Where-Object { $_.Thumbprint -eq $thumbprint } |
      Select-Object -First 1
    if ($found) {
      $storeCert = $found
      $storeName = $candidateStore
      break
    }
  }
  if (-not $storeCert) {
    Fail 3 "no certificate with thumbprint $thumbprint found in Cert:\CurrentUser\My or Cert:\LocalMachine\My."
  }
  return @{
    Route = "thumbprint"
    Thumbprint = $thumbprint
    Store = $storeName
    MachineStore = ($storeName -eq "Cert:\LocalMachine\My")
    CertSubject = $storeCert.Subject
  }
}

function Invoke-ProductionSigning {
  $timestampUrl = $env:COREVIDEO_SIGN_TIMESTAMP_URL
  if (-not $timestampUrl) { $timestampUrl = "http://timestamp.digicert.com" }

  if (-not (Test-Path $PackagePath)) {
    if ($DryRun) {
      Write-Host "[sign:native-msix] note: MSIX not found at $PackagePath (dry run continues; publisher read falls back to the repo manifest)." -ForegroundColor DarkGray
    } else {
      Fail 7 "MSIX not found at $PackagePath. Run npm run pack:native:msix first -- production signing never no-ops."
    }
  }

  $signTool = Find-SignTool
  if (-not $signTool) {
    Fail 2 ("signtool.exe not found. Install the Windows SDK (or set COREVIDEO_SIGNTOOL_PATH). " +
      "Production mode never exits 0 without signing.")
  }

  $plan = Resolve-ProductionRoute

  # --- Publisher match check (MSIX install fails on mismatch, so we fail first). ---
  $manifestInfo = Get-ManifestPublisher -MsixPath $PackagePath
  if (-not $manifestInfo) {
    Fail 4 "could not determine the AppxManifest Publisher (neither the MSIX nor native-shell/CoreVideoPro.WinUI/Package.appxmanifest was readable)."
  }
  $manifestPublisher = $manifestInfo.Publisher

  $certSubjectForCheck = $null
  $publisherCheck = "UNKNOWN"
  if ($plan.Route -eq "trusted-signing") {
    if ($plan.ExpectedPublisher) {
      $certSubjectForCheck = $plan.ExpectedPublisher
    } else {
      $publisherCheck = "SKIPPED"
      Write-Host "[sign:native-msix] WARNING: publisher match check SKIPPED -- COREVIDEO_SIGN_EXPECTED_PUBLISHER is not set for the Trusted Signing route." -ForegroundColor Yellow
      Write-Host "[sign:native-msix] WARNING: if the Trusted Signing cert subject differs from the manifest Publisher ('$manifestPublisher'), the signed MSIX will NOT install. Set COREVIDEO_SIGN_EXPECTED_PUBLISHER to enforce the check." -ForegroundColor Yellow
    }
  } else {
    $certSubjectForCheck = $plan.CertSubject
  }

  if ($certSubjectForCheck) {
    if ((Get-NormalizedSubject $certSubjectForCheck) -eq (Get-NormalizedSubject $manifestPublisher)) {
      $publisherCheck = "OK"
    } else {
      Write-Host ""
      Write-Host "[sign:native-msix] Publisher mismatch:" -ForegroundColor Red
      Write-Host "  Manifest Publisher: $manifestPublisher" -ForegroundColor Red
      Write-Host "    (from $($manifestInfo.Source))" -ForegroundColor DarkGray
      Write-Host "  Cert subject:       $certSubjectForCheck" -ForegroundColor Red
      Fail 4 ("the AppxManifest Publisher must EQUAL the signing cert subject or the MSIX will not install. " +
        "Update Publisher in native-shell/CoreVideoPro.WinUI/Package.appxmanifest (today: 'CN=CoreVideo Pro Dev') " +
        "to the cert subject above, repackage, and re-run.")
    }
  }

  # --- Print the signing plan. ---
  Write-Host ""
  Write-Host "[sign:native-msix] Production signing plan:" -ForegroundColor Cyan
  Write-Host "  Package:             $PackagePath"
  Write-Host "  SignTool:            $signTool"
  Write-Host "  Route:               $($plan.Route)"
  if ($plan.Route -eq "trusted-signing") {
    Write-Host "  Dlib:                $($plan.Dlib)"
    Write-Host "  Metadata:            $($plan.Metadata)"
  }
  if ($plan.Route -eq "pfx") {
    Write-Host "  PFX:                 $($plan.PfxPath)"
    $pwNote = "no (passwordless)"
    if ($plan.HasPassword) { $pwNote = "yes (value not printed)" }
    Write-Host "  PFX password:        $pwNote"
    Write-Host "  Cert subject:        $($plan.CertSubject)"
    Write-Host "  Cert thumbprint:     $($plan.CertThumbprint)"
  }
  if ($plan.Route -eq "thumbprint") {
    Write-Host "  Store:               $($plan.Store)"
    Write-Host "  Cert subject:        $($plan.CertSubject)"
    Write-Host "  Cert thumbprint:     $($plan.Thumbprint)"
  }
  Write-Host "  Timestamp (RFC3161): $timestampUrl"
  Write-Host "  Manifest publisher:  $manifestPublisher"
  Write-Host "    (from $($manifestInfo.Source))"
  Write-Host "  Publisher check:     $publisherCheck"

  # --- Build the signtool argument list. ---
  $signArgs = @("sign", "/fd", "SHA256", "/tr", $timestampUrl, "/td", "SHA256")
  switch ($plan.Route) {
    "trusted-signing" {
      $signArgs += @("/dlib", $plan.Dlib, "/dmdf", $plan.Metadata)
    }
    "pfx" {
      $signArgs += @("/f", $plan.PfxPath)
      if ($plan.HasPassword) {
        $signArgs += @("/p", $env:COREVIDEO_SIGN_PFX_PASSWORD)
      }
    }
    "thumbprint" {
      if ($plan.MachineStore) { $signArgs += "/sm" }
      $signArgs += @("/sha1", $plan.Thumbprint)
    }
  }
  $signArgs += $PackagePath

  if ($DryRun) {
    Write-Host ""
    Write-Host "[sign:native-msix] DRY RUN: signtool sign / verify NOT invoked. Plan resolved successfully." -ForegroundColor Green
    exit 0
  }

  # --- Sign (timestamping rides the same command; a timestamp failure fails it). ---
  Write-Host ""
  Write-Host "[sign:native-msix] signing (production, route=$($plan.Route))..." -ForegroundColor Cyan
  & $signTool @signArgs
  if ($LASTEXITCODE -ne 0) {
    Fail 5 ("signtool sign failed with exit code $LASTEXITCODE (this includes RFC3161 timestamp failures -- " +
      "the timestamp server $timestampUrl is REQUIRED; there is no un-timestamped fallback).")
  }

  # --- Verify. An artifact that does not verify is treated as unsigned. ---
  Write-Host "[sign:native-msix] verifying signature (signtool verify /pa)..." -ForegroundColor Cyan
  & $signTool verify /pa $PackagePath
  if ($LASTEXITCODE -ne 0) {
    Fail 6 "signtool verify /pa failed with exit code $LASTEXITCODE -- the artifact must not ship."
  }

  Write-Host ""
  Write-Host "[sign:native-msix] production-signed and verified:" -ForegroundColor Green
  Write-Host "  $PackagePath"
  exit 0
}

# ---------------------------------------------------------------------------
# Dev mode (preserved behavior: self-signed cert, exit 0 without signtool --
# but the unsigned outcome is now LOUD, never silent).
# ---------------------------------------------------------------------------

function Write-SigningDocs {
  Write-Host ""
  Write-Host "[sign:native-msix] Signing tools not available; document-only mode." -ForegroundColor Yellow
  Write-Host "  Env overrides (optional):" -ForegroundColor DarkGray
  Write-Host '    COREVIDEO_MSIX_PACKAGE_PATH  - package to sign' -ForegroundColor DarkGray
  Write-Host '    COREVIDEO_SIGN_CERT_SUBJECT  - cert subject (default: CN=CoreVideo Pro Dev)' -ForegroundColor DarkGray
  Write-Host '    COREVIDEO_SIGN_CERT_STORE    - store path (default: Cert:\CurrentUser\My)' -ForegroundColor DarkGray
  Write-Host "  1. Install Windows SDK (SignTool) or Visual Studio with Windows SDK components." -ForegroundColor DarkGray
  Write-Host "  2. Create a dev cert:" -ForegroundColor DarkGray
  Write-Host "     New-SelfSignedCertificate -Type Custom -Subject `"$CertSubject`" -KeyUsage DigitalSignature -FriendlyName `"CoreVideo Pro Dev`" -CertStoreLocation `"$CertStore`" -TextExtension @(`"2.5.29.37={text}1.3.6.1.5.5.7.3.3`")" -ForegroundColor DarkGray
  Write-Host "  3. Trust it: certmgr.msc -> Trusted People -> import the .cer export." -ForegroundColor DarkGray
  Write-Host "  4. Sign: signtool sign /fd SHA256 /a /n `"CoreVideo Pro Dev`" `"$PackagePath`"" -ForegroundColor DarkGray
  Write-Host "  An unsigned package retaining this publisher identity cannot be installed; sign it first." -ForegroundColor DarkGray
  Write-Host "  Production signing: re-run with -Mode production (see the env contract in this script's header)." -ForegroundColor DarkGray
}

function Write-DevUnsignedWarning {
  param([string]$Reason)
  Write-Host ""
  Write-Host "[sign:native-msix] WARNING: ARTIFACT LEFT UNSIGNED ($Reason). Dev mode tolerates this and exits 0; -Mode production would FAIL here. Do NOT ship this MSIX." -ForegroundColor Yellow
}

if ($Mode -eq "production") {
  Invoke-ProductionSigning
  # Invoke-ProductionSigning always exits.
}

if ($DryRun) {
  Write-Host "[sign:native-msix] note: -DryRun is a production-mode planning flag; dev mode ignores it and proceeds." -ForegroundColor DarkGray
}

if (-not (Test-Path $PackagePath)) {
  Write-Host "[sign:native-msix] MSIX not found at $PackagePath; run npm run pack:native:msix first." -ForegroundColor Yellow
  exit 0
}

$signTool = Find-SignTool
if (-not $signTool) {
  Write-DevUnsignedWarning "signtool.exe not found"
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
  Write-DevUnsignedWarning "SignTool failed with exit code $LASTEXITCODE"
  Write-SigningDocs
  exit 0
}

Write-Host ""
Write-Host "[sign:native-msix] signed package ready:" -ForegroundColor Green
Write-Host "  $PackagePath"
Write-Host "  Trust the cert under Local Computer -> Trusted People if installs are blocked." -ForegroundColor DarkGray
