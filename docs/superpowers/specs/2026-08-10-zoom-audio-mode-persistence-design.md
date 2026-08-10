# Persisting the Zoom→program audio mode

**Date:** 2026-08-10
**Status:** Approved, not yet implemented
**Follow-up to:** PR #397 (commit `0e19fcc`, "per-guest ISO vs Zoom program-mix mode"),
which shipped the mode as session-scoped and named persistence as a deliberate follow-up.

## Problem

`StudioViewModel._zoomAudioMode` chooses how Zoom audio reaches the program buses:

- **`ProgramMix`** (default, the long-standing Z1 topology) — Zoom's own echo-cancelled
  mix rides every program bus; guest strips meter only.
- **`PerGuestIso`** — each guest's isolated stem routes to program through their own
  strip, so faders/mutes/EQ shape program audio per guest, and zoom-mix is removed from
  the program buses (summing stems with the mix doubles every voice at the ~110ms
  measured Z1 internal echo).

The mode lives only in memory. Every launch resets to `ProgramMix`, so an operator who
runs their show in per-guest ISO has to re-flip the switch each time and has no signal
that they are back on the other topology.

## Scope decision: global

The setting is **global to the rig** — one value, applied regardless of which meeting is
joined. Per-meeting keying was considered and rejected: the ISO mode is a property of
*how the operator runs a show*, not of a particular meeting, and Zoom participant ids
already change every meeting, so a per-meeting key would buy a remembered topology and
nothing else.

## Storage: a string, not a bool

`ProductionOutputPreferences` gains:

```csharp
public string? ZoomAudioMode { get; set; }
```

with `CurrentVersion` bumped **8 → 9**.

A string rather than a bool, matching how the other enum-shaped prefs already persist
(`MultiviewLayoutMode`, `StreamSrtMode`, `RecordingQuality`, `RecordingFormat`):

- `ZoomAudioMode` is already a named enum in `ProductionModels.cs`. Persisting it as a
  bool would make the file describe a shape the code does not have, and a third topology
  later (e.g. mix + selected stems) would force a second migration.
- A string fails better. Parsing is case-insensitive; **any unrecognized or corrupted
  value falls back to `ProgramMix`** — the safe, current-behavior default. A bool cannot
  express "I do not recognize this."

Persisted values are exactly `"programMix"` and `"perGuestIso"` — the camelCased forms of
the `ZoomAudioMode.ProgramMix` / `ZoomAudioMode.PerGuestIso` members, matching the
camelCase convention the rest of the wire and prefs surface already uses. Not
secret-bearing: this is a topology choice, carrying no
credential, endpoint, path, or participant identity, so it needs no DPAPI treatment and
no support-bundle redaction test.

A standalone file (the `telemetry-consent.json` shape) was considered and rejected —
that file exists only to dodge the prefs-version race for *consent*. This is ordinary
operator state and belongs in prefs.

## Migration: v8 → v9

No explicit migration step. The existing tail of `ProductionOutputPreferencesStore.Load`

```csharp
if (preferences.Version < ProductionOutputPreferences.CurrentVersion)
{
    preferences.Version = ProductionOutputPreferences.CurrentVersion;
    migratedFromOlderVersion = true;
}
```

covers the bump, and an absent field deserializes to `null` → `ProgramMix`. Every
existing profile therefore behaves **byte-identically to today** on first load after the
upgrade. This matches the documented posture for the v4/v5/v6/v7/v8 bumps, none of which
needed explicit migration either; only the `Version < 3` local-audio-capture reset does,
and it stays pinned at `< 3`.

## Restore: the backing-field pattern

Restore happens in `ApplyProductionOutputPreferences` and writes the **backing field**
`_zoomAudioMode` directly — never `SetZoomAudioMode`.

This is the established house pattern (virtual camera O1, ISO-4, the mastering rack B2):
the public setter calls `TrySyncMediaCoreAsync`, and at preference-restore time the core
is not up yet. Writing the backing field lets the restored mode ride the first full sync
like every other piece of restored state.

The restore must also raise `PropertyChanged` for `ZoomAudioMode` and `IsPerGuestIsoAudio`
so the Audio-page toggle reflects the restored position. These are scalar properties, not
a bound collection, so they are outside the 0xc000027b churn rules.

## No core-respawn re-arm is needed

CLAUDE.md's rule — *"a one-shot command must be re-applied on every core generation"* —
would suggest this needs re-arming from `OnBridgeProfileChanged`. It does not, and the
reason is worth stating so nobody adds redundant machinery later:

`EnsureDefaultZoomAudioRoutingSends` runs on **every sync-context build**, synthesizing
its sends per sync rather than writing them into the matrix. The mode is therefore
continuously re-asserted, not sent once. A respawned core picks it up on the next sync
with no extra code. This is the same property that already makes mode flips self-cleaning
and makes new guests get covered automatically.

## Save

`SetZoomAudioMode` calls `StudioViewModel.SaveProductionOutputPreferences()` alongside its
existing `TrySyncMediaCoreAsync()` — the same one-line save every other operator-initiated
preference change uses (it is called from ~20 sites in `StudioViewModel` and via
`IShowInputsHost.SaveProductionOutputPreferences` from the roster coordinator). The
existing early-return when the mode is unchanged already keeps a no-op toggle from
rewriting the file.

## Honesty at launch

When the restored mode is `PerGuestIso`, restore sets the same `CommandStatus` line the
toggle already produces:

> Per-guest ISO audio: each guest routes to program through their own fader (Zoom's
> combined mix is off program).

Be precise about what this line does, because on its own it is weaker than "the operator
is told which topology they came up in" sounds. It is set in the constructor, at launch —
before Engine On, before a join, before any participant exists. `EnsureDefaultZoomAudioRoutingSends`
no-ops until `zoomParticipantIds.Count > 0` (`StudioViewModel.cs:8887-8890`), so `PerGuestIso`
has no audible effect until a meeting has guests, which in practice is minutes later. The
line therefore fires at the one moment the topology it announces cannot yet matter.
Separately, `CommandStatus` is written from roughly 50 call sites across the shell; the
happy-path launch does not clobber it, but ordinary pre-show setup — queueing a scene,
routing a source, assigning a role — will, well before the first guest joins. So the line
is real and honest (it never claims anything false), but it is a point-in-time status
write, not a durable indicator, and is likely gone from the screen by the time the mode it
describes actually engages. Restoring `ProgramMix` still says nothing — that is the
default, and a status line on every launch would be noise. See "Known residual /
follow-up" below for what closing this gap would take.

## Safety analysis

Persisting a topology that changes what the audience hears deserves an explicit argument
that it is safe. Three checks, all confirmed against current code:

1. **Per-guest mutes and faders do NOT persist.** `ProductionOutputPreferences` carries
   only a global `InputGainDb`; there is no per-participant mute or gain field. A
   restored ISO mode therefore cannot resurrect a muted guest — every launch starts with
   default strip state. This is the main reason persistence is safe at all.
2. **Launching into ISO mode with no meeting removes nothing.**
   `EnsureDefaultZoomAudioRoutingSends` returns `sends` unchanged when
   `zoomParticipantIds.Count == 0`, so no zoom-mix send is stripped from the program
   buses before a meeting exists.
3. **ISO mode creates no silent-program case that program-mix would not also have.** Both
   topologies depend on Zoom's recording privilege — the mixed stream and the isolated
   stems both arrive via `StartRawRecording`. Without the privilege both modes are silent,
   which is the condition PR #397's "Waiting for Zoom recording permission" status was
   added to make visible.

One residual, self-correcting difference: under THE FADER LAW a routed source with no
channel strip is dropped from the bus mix, so a guest whose stem is routed before their
strip is built is briefly dropped (loudly). Strips and the seeder both derive from the
same roster and the seeder runs every sync, so this converges within a sync cycle. It is
a property of ISO mode generally, not of persisting it.

## Known residual / follow-up

Persistence changes the operator-action shape of ISO mode: before this branch, running in
`PerGuestIso` required flipping the toggle every session, so the operator necessarily took
an action in the current session before zoom-mix left the program buses. After this
branch, a rig that restarts already in `PerGuestIso` reaches that same state — zoom-mix
stripped off every program bus the moment a guest joins — with no operator action in the
session at all. The only signal for it is the launch-time status line described above,
and that signal is launch-time only: it does not persist, is not re-asserted when the mode
actually starts affecting audio, and is commonly overwritten by ordinary pre-show setup
before that point is reached.

This branch does not close that gap; it ships correct persistence and storage, not a
stronger launch-time signal. Two shapes were identified for a follow-up, neither built
here: (1) re-assert the `CommandStatus` line at the moment `EnsureDefaultZoomAudioRoutingSends`
actually stops no-oping — i.e. when the first participant arrives — so the message lands
when it can matter; or (2) promote the indicator out of the transient `CommandStatus` line
entirely, into a persistent bound indicator in the `ShowInputWarning`/`RecordingDiskWarning`
pattern, so it survives being overwritten by unrelated status traffic. Either would close
the gap; which one (or both) is a product call outside this branch's scope.

## Testing

All in the existing suites — no rig, no meeting, no Mac:

**`ProductionOutputPreferencesStoreTests`**
- Round-trip: `perGuestIso` saves and loads back as `PerGuestIso`.
- A v8 file with no `ZoomAudioMode` field loads as `ProgramMix` and migrates to v9.
- An unrecognized value (`"chaos"`) falls back to `ProgramMix` rather than throwing.
- Case-insensitive parse (`"perguestiso"` → `PerGuestIso`).
- The serialized file contains `"Version": 9`.

**`StudioViewModelAudioStatusTests`** (where the existing mode tests live)
- Restore writes the backing field and does NOT trigger a core sync.
- Restore of `PerGuestIso` raises `PropertyChanged` for `ZoomAudioMode` and
  `IsPerGuestIsoAudio`, and sets the ISO `CommandStatus`; restore of `ProgramMix` sets no
  status.
- `SetZoomAudioMode` persists the new value.

The existing seeder tests (`RoutesTheMeetingMixNotIsoParticipants` and its ISO sibling)
already pin the routing behavior in both modes and must stay green — persistence changes
only where the mode comes from, never what it does.

## Out of scope

- Per-meeting or per-show scoping (rejected above).
- A third topology (mix + selected stems). The string encoding leaves room for it; nothing
  else here anticipates it.
- Persisting per-guest fader/mute/EQ state. That is a much larger question — Zoom
  participant ids are per-meeting handles, so it needs a stable identity scheme first —
  and it is what makes the current design safe by its absence.
- Any core, protocol, or snapshot change. This is shell-side state end to end.
