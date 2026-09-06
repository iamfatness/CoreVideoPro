# Architecture hardening migration

This change is additive to the existing JSON-line protocol. Windows and macOS
shells still launch the native core as a child process. Shared GPU/media surfaces
are separate from command transport.

## Output controls

Recording requests express intent. The core reports `starting` until a writer
successfully handles media, then `live`. Stop prevents new queued media and drains
the take through `stopping` and `finalizing`. Only `completed` with `finalized: true`
certifies successful writer completion. This still requires decode/quality checks
before a release artifact is certified. Errors remain visible after Stop.

Shells keep desired activity separately from the live indicator. Failed Stop
replies leave intent disarmed; later full-state updates must not restart output.
A core exit marks continuity interrupted and disarms recording/streaming. Starting
again creates a new recording session. The application does not silently claim
continuous output across a process crash.

Per-destination sender status remains the compatibility interface for streaming.
Starting is accepted but not live. A healthy remaining destination is not stopped
because another fails. RTMP `encoder-input-accepted` means media reached the local
FFmpeg input; it does not prove receipt by the remote server.

## Protocol and control clients

The handshake adds a process epoch and protocol major/minor version. Clients
accept absent version fields through the legacy adapter, accept additive major-1
minor versions, and reject explicit incompatible/invalid versions. Generated
lifecycle models require validation before use; see [wire rules](../contracts/README.md).

Legacy Zoom join calls retain their final response. Opt-in asynchronous callers
match both process epoch and operation ID on completion. A timeout does not prove
that an action failed. Do not replay an edge-triggered action such as Take merely
because its reply was lost. The bounded mailbox reports overload explicitly; see
[command ordering and limits](control-command-lifecycle.md).

LAN HTTP requires an authentication token. Loopback retains existing behavior.
OSC remains local unless both its separate LAN switch and trusted-network switch
are enabled. Bearer tokens over plain HTTP require a trusted network; use TLS or
an authenticated tunnel across an untrusted network. Companion setup is documented
in [its README](../companion-module-corevideopro/README.md).

## Saved configuration and releases

Production preference saves flush a same-directory temporary file and atomically
replace the primary while retaining a validated, protected backup. Missing files
are normal first launch; corrupt/unreadable files and backup recovery are distinct
outcomes. Recovery preserves the loaded configuration if a repair write fails.

Tag releases now depend on the reusable automated workflow and evidence bound to
the exact signed package and source commit. A separately provisioned controlled
rig supplies hardware evidence. Missing or invalid evidence blocks publication;
see [rig setup and evidence schema](release-evidence.md).

## Remaining migration

The generated lifecycle slice and native source-binding policy are foundations.
Legacy scene/audio/capture envelopes, durable participant identity, atomic native
Take, and outbound response backpressure remain explicit work in the
[ownership map](architecture-ownership.md) and [contract inventory](../contracts/README.md).
Live Zoom, physical fault injection, macOS adapter execution, two-hour A/V soak,
and clean-machine package validation are separate acceptance evidence, not claims
derived from unit test counts.
