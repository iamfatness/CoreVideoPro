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

Each action changes only its named destination. Other RTMP/SRT/NDI/HLS selections
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
