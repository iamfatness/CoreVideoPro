# WebView2 SDK (vendored)

Provenance: NuGet package `Microsoft.Web.WebView2` version **1.0.3800.47**
(https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.3800.47), downloaded
2026-07-12 from the nuget.org flat container
(`https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3800.47/microsoft.web.webview2.1.0.3800.47.nupkg`).

Vendored subset (only what `corevideo-browser-host.exe` needs):

- `include/WebView2.h` — COM API header (from `build/native/include/`)
- `include/WebView2EnvironmentOptions.h` — environment options helper (same dir)
- `x64/WebView2LoaderStatic.lib` — static loader (from `build/native/x64/`), so no
  `WebView2Loader.dll` has to be staged next to the host exe
- `LICENSE.txt` / `NOTICE.txt` — package license (BSD-style) and third-party notices,
  copied verbatim from the package root

The WebView2 **Runtime** (the evergreen browser itself) is NOT vendored — it ships
with Windows 11 / installs system-wide. `corevideo-browser-host.exe` probes for it at
startup (`GetAvailableCoreWebView2BrowserVersionString`) and fails loudly when absent.

To upgrade: download the newer `.nupkg`, replace the four files above, and update the
version + date in this file.
