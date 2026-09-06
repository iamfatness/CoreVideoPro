# companion-module-corevideopro

A [Bitfocus Companion](https://bitfocus.io/companion) module that remote-controls
**CoreVideo Pro** over its built-in HTTP/WebSocket control API.

- **Actions** are generated **live from the app's manifest** (`GET /manifest`), so the
  button set always matches the running app — transport (Take, record/stream/engine),
  scenes, Show Inputs (assign / name / in-show), lower-thirds & captions, audio
  monitor/limiter, multiview, and automation.
- **Feedbacks** (button lighting) and **variables** stream live over the app's
  `/ws` WebSocket — recording/streaming/engine/lower-third/automation flags, plus
  program/preview scene ids, statuses, multiview layout, etc.
- **Presets** for the common buttons (Take, Record, Stream, Engine, Lower-third,
  Auto-assign) are included.

## 1. Enable the control API in CoreVideo Pro

CoreVideo Pro starts its control servers automatically. By default they bind to
**localhost only**:

| Transport | Default | Env override |
|-----------|---------|--------------|
| HTTP + WebSocket | `127.0.0.1:8011` | `COREVIDEO_HTTP_PORT` |
| OSC (UDP) | `127.0.0.1:8010` | `COREVIDEO_OSC_PORT` |
| HTTP/WS LAN access | off | `COREVIDEO_HTTP_LAN=1` (binds `+`; may need a Windows `netsh http add urlacl`) |
| OSC LAN access | off | Both `COREVIDEO_OSC_LAN=1` and `COREVIDEO_OSC_TRUSTED_NETWORK=1` (binds `0.0.0.0`) |
| Bearer token | none on loopback; required for LAN HTTP/WS | `COREVIDEO_CONTROL_TOKEN` |

This module uses the **HTTP/WebSocket** transport. If Companion runs on a different
machine than CoreVideo Pro, set `COREVIDEO_HTTP_LAN=1` and a strong, non-blank
`COREVIDEO_CONTROL_TOKEN` on the app, then configure the same token in Companion.
Restart CoreVideo Pro after changing environment variables. LAN HTTP/WS refuses to
listen without a token; the launch log reports the configuration error.

HTTP requests use `Authorization: Bearer <token>`. WebSocket upgrades to `/ws` accept
that header or `?token=<URL-encoded token>`; ordinary HTTP routes never accept query
tokens. Tokens authenticate callers but plain HTTP does not encrypt them. Use LAN
control only on a trusted network; remote or untrusted-network access requires TLS
through a reverse proxy or an authenticated tunnel. Keep tokens out of URLs in logs.

HTTP LAN access does not enable OSC. OSC has no authentication: enable its two
separate settings only when every device on that network is trusted to operate the
show. Existing `COREVIDEO_OSC_LAN=1` configurations now keep both services local
until the new HTTP setting or explicit OSC trusted-network setting is supplied.

## 2. Build the module

```
npm install
npm run build      # tsc → dist/main.js
```

## 3. Sideload into Companion

1. In Companion, open **Settings → Developer modules path** and point it at the folder
   that contains this module directory.
2. Restart Companion. Add a **CoreVideo Pro** connection.
3. Configure **Host** (default `127.0.0.1`), **HTTP port** (`8011`), and the optional
   **Bearer token**.

When connected, the module fetches the manifest and populates actions; the connection
status turns green once the WebSocket is open.

## Protocol reference

The module speaks the app's control contract:

- `GET /manifest` → the action/feedback catalog (this is what generates the actions).
- `POST /invoke` → `{ "action": "<id>", "args": [ ... ] }` (positional, matching the
  manifest param order).
- `GET /state` → the current `ControlState`.
- `ws://…/ws` → a JSON `ControlState` snapshot on connect and on every change.

OSC clients can drive the same actions at `/cvp/<dotted.id.as/slashes>` on
`127.0.0.1:8010` with positional OSC args, and read feedback at `/cvp/state/<field>`.

## Notes

- Actions are regenerated whenever the WebSocket (re)connects, so restarting the app
  with a newer build refreshes the button set automatically.
- Modal actions in the app (file pickers) are intentionally not exposed remotely.
