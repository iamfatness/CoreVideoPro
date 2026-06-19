# Zoom Marketplace Credential & Scope Audit — CoreVideo Pro

Audit date: 2026-06-19
Scope: OAuth sign-in (Cloudflare Worker PKCE broker) + Meeting SDK join path, for Marketplace
publishing readiness. Goal: prevent the OAuth/Meeting-SDK credential & scope issues that blocked
the sibling app.

## How to read this

- **PASS** — verified from repo source (file:line) and/or the live broker.
- **FAIL** — verified-broken; remediation given.
- **CONFIRM** — cannot be proven from this repo alone (broker secrets live in the sibling
  `iamfatness/CoreVideo` worker, and dev-vs-prod identity only exists in the Zoom Marketplace
  dashboard). Action required before publishing.

The three credentials must not be conflated:
1. OAuth **Client ID** (confidential) → needs Client Secret at token exchange.
2. **Public Client ID** (PKCE/public) → no secret; correct choice for a desktop app.
3. **Meeting SDK Client ID** + SDK Secret → mints the Meeting SDK JWT for joining.

The single embedded value used everywhere is `y6sIWSwiTZe1JygMx4C9EQ`.

---

## Live evidence

`GET https://corevideo.iamfatness.us/oauth/start?state=test&return_uri=corevideo://oauth/callback`
returns `302` to:

```
https://marketplace.zoom.us/v2/authorize
  ?response_type=code
  &client_id=y6sIWSwiTZe1JygMx4C9EQ
  &redirect_uri=https%3A%2F%2Fcorevideo.iamfatness.us%2Foauth%2Fcallback
  &state=...
  &code_challenge=...&code_challenge_method=S256
  &scope=user:read:zak user:read:user user:read:token
```

Key facts: PKCE is used (`code_challenge` + `S256`), the broker requests **`user:read:token`**, and the
authorize `client_id` is `y6sIWSwiTZe1JygMx4C9EQ`.

---

## Checklist

### 1. OAuth Client ID (sign-in)

| # | Check | Verdict |
|---|-------|---------|
| 1a | Broker authorize `client_id` matches the build's intended public client id | **PASS** |
| 1b | Client is configured as **Public/PKCE** (no secret at token exchange) | **PASS (live) / CONFIRM (dashboard)** |
| 1c | `client_id` is the PRODUCTION value (not Development) | **CONFIRM** |
| 1d | `client_id` is the *Public* OAuth id, not the confidential OAuth Client ID | **CONFIRM** |

- **1a PASS** — live authorize `client_id=y6sIWSwiTZe1JygMx4C9EQ` equals the embedded value in
  `src/config/zoomOAuth.json:5`, `native-shell/CoreVideoPro.MediaCore/Services/ZoomOAuthManifest.cs:10`,
  and the test pin `src/config/zoomOAuth.test.ts:18`.
- **1b** — Live authorize sends a PKCE `code_challenge`, and the desktop token-exchange path posts a
  `code_verifier` with **no `client_secret`**
  (`native-shell/CoreVideoPro.MediaCore/Services/ZoomOAuthService.cs:167-184`). For the broker path the
  Worker redeems server-side (`/oauth/redeem`, ZoomOAuthService.cs:138-159), so the **broker** must be
  pointed at a Public client (or hold the secret itself). Confirm in the broker source/secrets.
- **1c / 1d CONFIRM** — Whether `y6sIWSwiTZe1JygMx4C9EQ` is the **Production Public** Client ID (vs the
  Development one, vs the confidential OAuth Client ID) cannot be determined from outside Zoom. This is
  the exact failure mode that yields `Invalid client_id or client_secret` at token exchange. **Verify in
  Zoom Marketplace** that this id is the published app's Public (PKCE) client id.

### 2. OAuth Scopes (the ZAK breaker)

| # | Check | Verdict |
|---|-------|---------|
| 2a | Broker REQUESTS `user:read:token` in the authorize URL | **PASS** |
| 2b | The Zoom app GRANTS `user:read:token` on the same app | **CONFIRM** |

- **2a PASS** — live authorize `scope` includes `user:read:token` (plus `user:read:user` and the
  harmless legacy `user:read:zak`). The ZAK fetch correctly hits
  `https://api.zoom.us/v2/users/me/token?type=zak` (ZoomOAuthService.cs:423), which requires
  `user:read:token` — **not** `user:read:zak`.
- **2b CONFIRM** — A requested scope is not a granted scope. Confirm the Marketplace app's **Scopes**
  tab lists `user:read:token` as added/approved on the SAME app whose client_id the broker uses.
- **Operator note (mandatory after any scope change):** the user MUST revoke/uninstall the app in their
  Zoom account and re-consent. Otherwise Zoom reissues a cached access token without the new scope and
  ZAK fetch keeps failing. Repo config drift to watch: `src/config/zoomOAuth.json:6` lists only
  `user:read:token user:read:user` (no `user:read:zak`) — harmless because the **broker** is the source
  of truth for the authorize scopes, but keep them aligned to avoid confusion.

### 3. Meeting SDK (join)

| # | Check | Verdict |
|---|-------|---------|
| 3a | Broker `ZOOM_MEETING_SDK_CLIENT_ID` == build's embedded `publicAppKey` | **CONFIRM** |
| 3b | Broker `ZOOM_MEETING_SDK_CLIENT_SECRET` is that app's PRODUCTION secret | **CONFIRM** |

- Embedded `publicAppKey` is `y6sIWSwiTZe1JygMx4C9EQ`, consistent across `src/config/zoomMeetingSdk.json:2`,
  `native/src/config/ZoomMeetingSdkConfig.h:6`, and the native fallback
  (`native/src/modules/ZoomEngineRuntime.cpp:73-75`).
- When signed in, the broker mints the SDK JWT via `POST /oauth/sdk-jwt`
  (ZoomOAuthService.cs:442-463) using its `ZOOM_MEETING_SDK_CLIENT_ID/SECRET` Worker secrets; the native
  engine then auths with that JWT (`native/zoom-engine/engine/main.cpp:1225-1244`). The embedded
  `publicAppKey` is only the un-signed-in fallback.
- **3a/3b CONFIRM** — Both the SDK Client ID and Secret are Worker secrets in the sibling repo and are
  not visible here. Confirm: (i) broker `ZOOM_MEETING_SDK_CLIENT_ID` exactly equals
  `y6sIWSwiTZe1JygMx4C9EQ`, and (ii) `ZOOM_MEETING_SDK_CLIENT_SECRET` is that app's **Production** SDK
  secret (not a dev secret). A dev-id/prod-secret (or id≠publicAppKey) mismatch authenticates the join
  against the wrong app and yields `AUTHRET_KEYORSECRETWRONG`.
- **Note on the shared id:** OAuth `client_id` and the Meeting SDK key are the same string. This is only
  correct if the Marketplace app is a unified **General App** with BOTH the Meeting SDK feature and
  Public-OAuth enabled, sharing one Client ID. If they are actually two separate apps, this is the
  conflation that breaks publishing. **Confirm in the dashboard** which case applies.

### 4. Dev vs Prod

| # | Check | Verdict |
|---|-------|---------|
| 4a | No Development credential baked into the shipped build / CI / docs | **PASS (in-repo) / CONFIRM (identity)** |

- No competing/dev credential value appears anywhere in the repo — `y6sIWSwiTZe1JygMx4C9EQ` is the only
  Zoom id present, and CI (`.github/workflows/ci.yml`, `release.yml`, `deploy-demo.yml`) injects no Zoom
  credentials. The `native/build-dev` references in `scripts/validate-live-zoom.mjs` and
  `scripts/test-zoom-auth.mjs` are local dev *build output paths*, not credentials.
- **CONFIRM** — Because the single embedded id cannot be classified as Dev or Prod from outside, item 1c
  is the gate: confirm `y6sIWSwiTZe1JygMx4C9EQ` is the **Production** value, and that the broker's
  Worker secrets (OAuth + SDK) are also the Production set.

### 5. Secret hygiene

| # | Check | Verdict |
|---|-------|---------|
| 5a | No client/SDK secret committed to the repo | **PASS** |
| 5b | No secret baked into the desktop binary | **PASS** |

- No secret literals in the repo; the only secret plumbing reads from gitignored
  `scripts/.staging-secrets.local` (`.gitignore:32`, `scripts/deploy-staging-workers.ps1`) and pushes via
  `wrangler secret put`. The desktop/native build embeds only the **public** app key
  (`ZoomMeetingSdkConfig.h:6`); the SDK secret never leaves the broker.

---

## FAIL / fix-now items

### F1 — Deep-link return scheme mismatch (functional FAIL)

- Code registers/returns **`corevideo://oauth/callback`**
  (`src/config/zoomOAuth.json:4`, `ZoomOAuthManifest.cs:9`), but the docs/runbook instruct operators to
  register **`corevideopro://oauth/callback`**
  (`docs/zoom-windows-sdk-packaging.md:49`, `docs/operator-validation-runbook.md:138,146,151,250`).
- Impact: the browser→app handoff after consent fails if the registered scheme doesn't match what the
  app passes as `return_uri`.
- **Remediation:** pick one scheme and make code, the WinUI protocol registration, and docs identical.
  (No live credential change — safe to fix once you choose the scheme.)

---

## Pre-publish CONFIRM list (owner action, do NOT change live values without sign-off)

1. In Zoom Marketplace, confirm `y6sIWSwiTZe1JygMx4C9EQ` is the **Production Public** OAuth Client ID
   (PKCE), not the Development id and not the confidential OAuth Client ID. (items 1c/1d/4a)
2. Confirm the app is a single **General App** exposing both Meeting SDK and Public-OAuth on that one
   client id — or, if two apps, split the broker config accordingly. (item 3 note)
3. Confirm the **Scopes** tab grants `user:read:token` on that same app. (item 2b)
4. In the broker (sibling `iamfatness/CoreVideo` Worker), confirm the secrets are the **Production** set:
   the OAuth leg uses the Public/PKCE client (no OAuth client_secret needed, or the secret is present if
   confidential), and `ZOOM_MEETING_SDK_CLIENT_ID == y6sIWSwiTZe1JygMx4C9EQ` with its matching
   **Production** `ZOOM_MEETING_SDK_CLIENT_SECRET`. (items 1b/3a/3b)
5. After any scope/credential change, **revoke + re-consent** in the test Zoom account before
   re-validating ZAK. (item 2 operator note)
