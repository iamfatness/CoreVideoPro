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
- **macOS receives the callback on loopback, not the scheme (2026-11-15).** The broker
  also accepts `http://127.0.0.1:<port>/oauth/callback` / `http://localhost:<port>/oauth/callback`
  (path exactly `/oauth/callback`; `isAllowedOauthReturnUri`). The OBS plugin product registers
  `corevideo://` too (`CoreVideoOAuthCallback.app`), and on the owner's Mac LaunchServices made
  it the default handler, so the signed 1.0.0 Pro never finished signing in. The mac shell
  (`mac-shell/.../ZoomOAuthLoopback.swift`) starts a one-shot `NWListener` on 127.0.0.1 (ephemeral
  port, never the 8011 control server) at each Sign in with Zoom, answers exactly one
  `GET /oauth/callback` (anything else 404s without consuming it), stops after the callback /
  5 minutes / a new sign-in, and never logs the query. `corevideo://` stays as the fallback
  (`.onOpenURL`) when the listener cannot bind. The Windows shell still uses the scheme.
