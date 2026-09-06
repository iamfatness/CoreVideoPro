# Additive lifecycle contract slice

`lifecycle.schema.json` is the source of truth for protocol version, output
lifecycle, asynchronous operation status and structured protocol failure objects.
`npm run contract:generate` emits checked-in C++, C#, browser TypeScript, Node
TypeScript and Swift models plus validators. `npm run contract:check` and CI
reject stale generated output. The two TypeScript outputs are generated identically
so the Node package preserves its `rootDir: src` build boundary.

The schema is deliberately a small first slice. It does **not** generate the entire
legacy protocol or replace its envelope/dispatch adapters. `lifecycle.fixtures.json`
contains identical raw wire messages for all language suites. Tests cover required
and optional fields, explicit null, booleans, integer bounds and decimal notation,
unsupported major versions, unknown enum values, and additive object fields.
C# and Swift also exercise typed decoding/encoding after validation; C++ exercises
its generated serializer and JSON validators. Legacy parity string tests remain
until their message families gain serialized-message tests.

Wire rules:

- Field names are case sensitive. Additive object fields are accepted and may be
  discarded by typed models. Clients must not rewrite unknown fields to persist
  a newer client's complete document.
- Required fields cannot be absent or null. Optional `error` may be absent;
  explicit null is invalid. Serializers omit absent optional fields.
- Integer fields use signed 32-bit bounds specified by the schema. JSON numeric
  notation such as `1.0` is a valid integer; fractions and overflow are invalid.
- Unknown lifecycle/health/operation enums fail validation. A consumer should
  display unknown/unverified state and report incompatibility, never coerce an
  unknown enum into live/success. An unknown additive field is different from an
  unknown value in a closed enum.
- Call the generated runtime validator **before** using a decoded object. DTO
  deserialization alone does not enforce every enum or semantic constraint.
- Protocol major 1 is supported; higher minor versions remain additive. Legacy
  messages without these new objects pass through explicit legacy adapters.

## Remaining supported protocol families

| Family | Current handwritten owners | Next coverage boundary |
| --- | --- | --- |
| RPC envelopes, hello/capabilities, command acknowledgements | `native/src/rpc/JsonRpcServer.cpp`, `src/engine/nativeBridgeProtocol.ts`, C# client, Swift bridge | Envelope IDs, required fields, response/error unions |
| Scene graphs, preview, tiles, overlays, backgrounds, media playback | `MediaCore.h/.cpp`, `nativeMediaCoreProtocol.ts`, C#/Swift scene builders | Route modes, coordinate fields, nullability, atomic scene batch |
| Show inputs, participant roster, Zoom source/subscription/spine | `ZoomEngineRuntime`, `zoomMediaSpineSync.ts`, C#/Swift Zoom models | Durable identity vs session ID, partial roster updates, subscription limits |
| Recording/streaming configuration and full output telemetry | Encoder/sender interfaces, core snapshots, shell snapshot DTOs | Per-destination identity, writer stats, artifact/finalization proof |
| Audio buses, mixer, DSP/VST, device routing | Native audio module DTOs and shell builders | Numeric units/ranges, topology, plugin state |
| Capture and frame transport | Native capture/shared-texture messages and platform bridges | Handle ownership, dimensions/strides, timestamps, process epoch |
| Diagnostics, support, licensing, automation/control | Core/control servers and shell view models | Redacted diagnostics, action idempotency, compatibility capabilities |

Do not declare full generated-contract coverage until these families have their
own schemas, golden fixtures, runtime validation and tested legacy adapters.
