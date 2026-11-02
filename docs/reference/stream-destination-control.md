# Stream destination control

`GET /manifest` lists the installed shell's supported actions and positional
parameters. The HTTP API belongs to the shell, not the native media-core process.
Beta `beta-2026-10-16-079e79c` exposes stream start/stop but does not yet include
the configuration actions below. Editing its preferences file while running does
not update its in-memory settings.

The new actions use `POST /invoke`, on the same loopback/authenticated control
server as other actions. They configure a destination without starting streaming:

```json
{"action":"transport.stream.rtmp.configure","args":[true,"rtmps","rtmps://example.test/live","YOUR_KEY"]}
```

RTMP arguments: enabled, protocol (`rtmp`/`rtmps`), server URL, optional stream key.
Credentials belong in the key; this action rejects user info, query or fragment in
the server URL. Omit the key or pass null to keep the current value. Empty clears
it; an enabled RTMP destination must have a valid key and application URL.

```json
{"action":"transport.stream.srt.configure","args":[true,"caller","example.test","9000","120","","0",""]}
```

SRT arguments: enabled, mode (`caller`/`listener`/`rendezvous`), host without a URL
scheme, port (1–65535), latency in milliseconds (at least 20), stream ID (may be
empty), key length (`0`/`16`/`24`/`32`), optional passphrase. Omitted/null passphrase
keeps the current value; empty clears it. With encryption enabled, the existing
validation requires 10–79 characters. Disabling encryption requires clearing the
passphrase. Numeric fields accept strings, matching the bound settings controls.

NDI is not a stream destination. It has its own on/off and actions; see
[NDI output](#ndi-output) below. The NDI graphics key (fill+key) is controlled by
`transport.ndi.graphicsKey.set`; see [ndi-graphics-key.md](ndi-graphics-key.md).

Each action changes only its named destination. Other RTMP/SRT/HLS selections
remain as configured. To switch from RTMP to SRT, disable RTMP with its configure
action before starting the stream; configuration is not exclusive destination
selection. Disabled destinations may retain incomplete drafts. Existing stream
start validation still checks all enabled destinations and runtime availability.

Requests run on the UI dispatcher and are refused while streaming is requested,
observed active, transitioning, or closing. Full validation precedes persistence;
the encrypted preferences store commits before bound properties change. A failed
save leaves the current settings untouched and returns an error. Batched setters
do not trigger intermediate saves or output synchronization. The next ordinary
stream start uses the new settings without restarting the app or meeting.

Success replies contain only `ok`, never destination credentials. Errors do not
echo argument values or persistence exception details. Stream keys, SRT stream
IDs and passphrases use the existing DPAPI protection at rest. Clients should
avoid writing credential-bearing request bodies into their own logs.

Qualification: `CoreVideoPro.WinUI.exe --verify-stream-destinations <report.json>`
runs the HTTP router and real UI-dispatched control adapter against an isolated
ViewModel with an in-memory preference store and interactive startup disabled.
It checks single-commit RTMP/SRT updates, current UI state, write-failure recovery,
and requested/observed streaming refusal. It opens no visible window, listener,
media core, meeting or production preferences. Focused tests separately cover
DPAPI file persistence, invalid settings, omitted/cleared secrets and safe errors.

## NDI output

NDI output is independent of Streaming and Recording. It runs with or without
either, and starting, stopping or editing a stream destination never restarts it
(the core keeps one writer per destination; adding or removing `rtmp` does not
touch `ndi`).

- **On/off.** `NdiOutputEnabled`, persisted in the output preferences (schema v16,
  default off). The v15 `StreamNdiEnabled` value is ignored on upgrade: NDI is off
  after upgrading until the operator turns it on. Controls: the NDI tile in the
  Studio footer (next to Virtual Cam; its flyout holds program name, group, graphics
  key and key name, text fields committing on LostFocus/Enter) and the "NDI output"
  switch in Settings, NDI.
- **Format.** The stream output profile (resolution, fps); there is no separate NDI profile.
- **Sync.** Whenever NDI is on, the production sync sends `ndi`, plus `ndi-graphics`
  when the graphics key is on.
- **Validation.** NDI validates alone (`ValidateNdiOutput`): program name, key-name
  collision and the `ndi-output` core capability. Stream validation, the start proof,
  failures, the STREAM readout, stream summaries and the close guard ignore NDI
  (`OutputDestinationKind.IsStream`). A failure while NDI runs turns NDI off and
  records `NdiOutputStatusReason`; it never touches the stream. NDI on does not
  prompt on close, like the virtual camera.
- **Status strings.** `Off`, `Starting`, `Live · {name}`, `Warning · {reason}`,
  `Unavailable · {reason}`, each with ` + key` appended when the `ndi-graphics`
  sender is live.
- **Control API.** `transport.ndi.set` (`on`: bool, required), `transport.ndi.toggle`
  (no arguments), `transport.ndi.graphicsKey.set` (`on`, optional `name`). Control
  state: `ndiOutputOn`, `ndiOutputStatus`. OSC feedback: `/cvp/state/ndi` (0/1).
  `transport.stream.ndi.graphicsKey.set` was removed; use `transport.ndi.graphicsKey.set`.
