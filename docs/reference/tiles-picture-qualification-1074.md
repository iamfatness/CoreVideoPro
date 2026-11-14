# Tiles picture qualification — #1074

The implemented policy retains a stalled Zoom member's slot for four seconds, labels it held after 1.5 seconds, and removes video-off/departed members immediately. Local static sources are exempt from this Zoom age rule. Existing tests drive actual MediaCore ticks and verify membership, plan geometry and truthful black-versus-held metadata. They do not inspect the D3D11 picture produced from those decisions.

## Scope and acceptance

Drive controllable Zoom-kind sources through the production source bus and MediaCore gather, with the real D3D11 compositor. Give sources distinct opaque colors and a changed picture on recovery. Read back the actual rendered result at the center of each admitted tile, away from borders and labels. Assert renderer identity and nonempty GPU output before judging pixels. A software-plan signature or synthetic preview is not this evidence.

The live picture must appear in its assigned slot. During a 1–3 second frozen frame-ID interval, the hold-policy picture and slot must remain; the black-policy member must keep its slot but paint black after the bus's real-time stale threshold. Recovery must restore the changed picture without duplicating or exchanging people. A stall beyond four seconds must remove the member under the current policy. Video-off/departure must remove it on the next gather even inside the hold window. Keep existing local-static-source regressions.

Use literal behavioral thresholds in assertions rather than deriving expectations from the constants under test. Preserve a negative control that reinstates premature removal or suppresses black painting and demonstrate that the new picture check fails. Do not change the four-second policy, stream bitrate, source resolution or output buffer to pass.

## Execution and limits

Reuse the existing controllable source and render-plan fixtures where possible. Add a focused actual-GPU test with bounded readiness and explicit test-only readback. Run it after measured live recording ends, then run affected Tiles/native gates. Retain exact executable identity and result logs. If an output defect is found, retain its failing case before a focused correction.

This is local GPU behavior qualification, not a real-network interruption rehearsal, a full output-delivery pass, or an owner ruling on the four-second product choice. The populated test meeting does not give us control over remote senders. Do not infer a network cause from coincident stalls or call the whole issue closed without its remaining owner acceptance.
