# Secrets at rest + OAuth return URI (beta S4, 2026-07-18)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

- **Credentials at rest use DPAPI** via `DpapiSecretProtector` (WinUI, CurrentUser
  scope, `"dpapi:"+base64` field-level format): Zoom OAuth tokens
  (`FileZoomTokenStore` encrypt/decrypt delegates) and the RTMP stream key / SRT
  passphrase in `production-output-preferences.json` (prefs schema **v4**). Legacy
  plaintext files load fine and re-save encrypted on first load (never lose a working
  token). Any NEW persisted credential must ride the same delegates — never write a
  secret plaintext, and give every new secret-bearing bundle field a redaction test
  (`SupportBundleExportTests` is the template).
- **The OAuth app-return URI is `corevideo://oauth/callback` and is broker-pinned**:
  the deployed broker (`corevideo.iamfatness.us`, `site-worker.js handleOauthStart` in
  the external CoreVideo repo) 400-rejects any other `return_uri`. `corevideopro` is a
  legacy protocol alias only. Don't change the scheme without updating the broker
  allowlist first; `ZoomOAuthManifestTests`/`ZoomOAuthProtocolTests` pin it.
