# EngineHost committed production intent (#961)

Build on the opt-in EngineHost from #960 and the authority contract in `docs/reference/engine-ui-crash-survival-plan.md`. The console retains edit drafts and UI projection. Native remains authoritative for applied routes, capture and outputs. The host owns the committed inputs needed to continue production when the console disconnects.

## Ownership cut

- **Spine synchronization.** Today `TransportCoordinator` configures the bridge timer and supplies a factory that captures WinUI state on the dispatcher. `BuildSpinePayload` mixes committed route, audio and capture intent with changing roster facts and the console's multiview layout. Separate committed inputs from current native observations and UI presentation. Adopt the first bounded host-owned consumer only after a fenced, accepted intent update. Keep a single periodic producer in hosted mode and keep the legacy path available.
- **Routes, assignments and audio.** Trace ShowInputs, Transport and mixer changes from command admission through native acknowledgement. Store the accepted intent and epoch in the host. Keep draft settings and meter refresh in WinUI. Preserve source incarnations and invalidate assumptions on core restart.
- **Automation and ingress.** MagicScene depends on a UI timer. Native graphics already evaluates armed rules, while the shell handles configuration and some guest values. Move supported production dispatch into the host and report any enabled behavior that still depends on WinUI. HTTP and OSC currently live in MainWindow and call StudioControlSurface. Inventory Companion and OHG consumers and route supported production actions through the same revisioned dispatcher. Keep navigation actions in WinUI.
- **Recovery.** MediaCoreSupervisor owns the child process. Studio currently handles the rejoin outcome and output resume decision. Move that continuation into the host, keep recording and streaming prerequisites distinct, and cancel pending recovery on explicit Stop. A console loss or reattach must never trigger recovery.
- **Deferred boundary.** Compatibility camera fallback moves under #962. Default enablement and packaged recovery qualification remain under #963.

## Verification sequence

1. Write a deterministic test that commits a spine input, detaches the console, then advances the host tick and observes the updated native command. Cover stale writer fences, an old core epoch, and a rejected command. Extend the opt-in host's real dispatch path for this first consumer.
2. Test accepted routing, assignment and audio edits across lost ACK and reattach. Assert that the returning console first adopts native and host revisions without replaying stored preferences.
3. Exercise the production control surface and supported automation while the UI is absent. Verify guest join, leave, mute and ID reuse with the native incarnation contract.
4. Test core restart and explicit Stop ordering. Assert applied output state, not only a sent command or living process. Verify a controlled meeting before claiming #961 acceptance.

## Initial subscription handoff

While the console is sending spine updates, the existing producer remains active. After it goes quiet, EngineHost can refresh subscriptions from the last input that native accepted for the same roster. The host reads a fresh roster and sends an engine-only refresh. It cannot apply Preview or multiview layouts, re-arm stopped capture, or adopt a replacement guest under a reused SDK ID.

A different core process, meeting epoch or capture generation invalidates the input. A newly admitted production mutation clears the pending continuation until another spine input is accepted. Batches containing only the revisioned audio route and monitor controls preserve it. These controls affect the native mixer without changing subscription inputs. Native retains the applied audio revision across detach. The native check also compares the observed roster revision to the current revision, so a queued refresh cannot apply obsolete roster facts after a join or leave. The urgent command lane remains available.

This change covers the existing opted-in host. Reattached Studio remains read-only. Full host ownership of committed intent, control ingress, automation and core restart handling still requires the subsequent work above.