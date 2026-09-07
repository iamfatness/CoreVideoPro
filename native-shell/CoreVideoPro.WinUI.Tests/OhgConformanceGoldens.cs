namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The pinned facade sequence each <c>HOST_CONFORMANCE_CASES</c> entry must produce when its host
/// commands are pushed through the real <see cref="CoreVideoPro.WinUI.Services.OhgHostAdapter"/>
/// over a <see cref="RecordingOhgHostFacade"/> (Plan 7a Task 13, spec §11's "conformance" row).
///
/// <para><b>These are not "whatever it printed".</b> Each entry was read against the case's stated
/// intent in <c>show-engine/src/conformance.ts</c> before being pinned — the intent is quoted on the
/// entry. A sequence that contradicted its case's intent would be a Task 10/11 adapter bug and had
/// to be FIXED, never pinned; that check is the only reason this table is worth having, because the
/// engine-side suite asserts on what a RECORDER received and therefore passes just as happily
/// against an adapter that forwards nothing.</para>
///
/// <para><b>Two things every case shows that the engine-side suite never mentions</b>, both read
/// against the spec rather than accepted from the output:</para>
/// <list type="bullet">
/// <item><description>A <c>setGallery</c> lands on the FIRST tick of every case (the engine
/// publishes the whole 16-cell map, all blank, as soon as it has a roster). Spec §8 says
/// <c>setGallery</c> is <b>not applied in 7a</b> — recorded, with one status line — so the only
/// facade traffic it may produce is that single <c>ReportStatus</c>, once per adapter. Anything
/// that MOVED a tile here would be the adapter doing what 7a explicitly does not do.</description></item>
/// <item><description>The engine emits <c>setPreview({kind:"look"})</c> <b>before</b> the
/// <c>applyLook</c> that first names that look's scene preset, so the adapter — which can only
/// learn <c>lookId → scenePreset</c> from <c>applyLook</c> (spec §8) — refuses it. That refusal is
/// pinned in <see cref="AdapterConformanceTests"/> as a KNOWN DEFECT, not accepted as correct; see
/// the comment there. It produces no facade call, which is why it is invisible in this table.</description></item>
/// </list>
///
/// <para><b>What a golden is sensitive to.</b> The sequence is the adapter's own conversion work, so
/// it reds when the adapter mis-maps: swapping <c>hostSlot</c>/<c>readerSlot</c> moves the look
/// case's <c>ohg-host</c>/<c>ohg-reader</c> route values; dropping <c>assignSlot</c> forwarding
/// empties the slot bindings out of every case. It is deliberately NOT sensitive to how the engine
/// phrased a command — that is the engine's own suite's job.</para>
///
/// <para><b>Recording shapes</b> come from <see cref="RecordingOhgHostFacade"/>: one
/// <c>"&lt;Method&gt;(&lt;args&gt;)"</c> per call, route dictionaries rendered with keys sorted
/// ordinally so a <c>Dictionary</c>'s insertion order can never decide whether a test passes.
/// <c>SceneExists</c> and <c>CanTake</c> are recorded too — they are how the adapter decides whether
/// to refuse a whole command, so a silent change from "checked, then acted" to "acted" matters.</para>
/// </summary>
internal static class OhgConformanceGoldens
{
    /// <summary>The eight <c>AssignZoomParticipant</c> calls every case's first tick produces —
    /// three seated, five empty — plus the once-per-adapter gallery note that rides the same tick's
    /// <c>setGallery</c>. Shared because it is the SAME evidence in all six cases (the cases all
    /// seat the same cast first), and a reader comparing two goldens should be comparing what
    /// differs, not re-reading nine identical lines.</summary>
    private static readonly string[] SeatingPreamble =
    {
        "AssignZoomParticipant(1, c1)",
        "AssignZoomParticipant(2, c2)",
        "AssignZoomParticipant(3, c3)",
        "AssignZoomParticipant(4, null)",
        "AssignZoomParticipant(5, null)",
        "AssignZoomParticipant(6, null)",
        "AssignZoomParticipant(7, null)",
        "AssignZoomParticipant(8, null)",
        "ReportStatus(gallery: cell order not applied (Tiles has no explicit order API))"
    };

    /// <summary>The full placement of the conformance look with box 1 holding slot 3 — the shape the
    /// look case asserts on, expressed once because two cases produce it.</summary>
    private static readonly string[] FullLook =
    {
        "SceneExists(conformance-scene)",
        "CueSceneWithRoutes(conformance-scene, ohg-box-1=3, ohg-box-2=null, ohg-host=1, ohg-reader=2)",
        "SetInputDisplayName(1, Cara Ames)",
        "SetInputLowerThirdTitle(1, Oslo)",
        "SetInputDisplayName(2, Dev Blake)",
        "SetInputLowerThirdTitle(2, Reno)",
        "SetInputDisplayName(3, Eve Cole)",
        "SetInputLowerThirdTitle(3, Lima)",
        "SetCaption()"
    };

    /// <summary>The same look with an EMPTY box 1: both guest routes carry nobody and the plates
    /// name only the two chairs.</summary>
    private static readonly string[] EmptyBoxLook =
    {
        "SceneExists(conformance-scene)",
        "CueSceneWithRoutes(conformance-scene, ohg-box-1=null, ohg-box-2=null, ohg-host=1, ohg-reader=2)",
        "SetInputDisplayName(1, Cara Ames)",
        "SetInputLowerThirdTitle(1, Oslo)",
        "SetInputDisplayName(2, Dev Blake)",
        "SetInputLowerThirdTitle(2, Reno)",
        "SetCaption()"
    };

    private static string[] Sequence(params string[][] parts) => parts.SelectMany(part => part).ToArray();

    /// <summary>case name → the facade calls that case must produce, in order.</summary>
    internal static readonly IReadOnlyDictionary<string, string[]> Expected =
        new Dictionary<string, string[]>(StringComparer.Ordinal)
        {
            // INTENT: "Slot binding. … one call per slot, emitted when that slot's binding CHANGES
            // and never as a full sweep." Read out: the first tick binds all eight slots, and
            // removing the panelist in slot 2 re-binds SLOT 2 ALONE to null — no re-sweep of the
            // seven slots that did not move. That is the whole roster contract, and the adapter
            // carries it through unchanged.
            ["a seated participant binds its slot on the host"] = Sequence(
                SeatingPreamble,
                new[] { "AssignZoomParticipant(2, null)" }),

            // INTENT: "`applyLook` is ONE call carrying the whole placement — the look id, the scene
            // preset it renders through, both chairs, and the guest boxes — so a host never
            // re-derives `lookId -> scenePreset` or hunts the chairs out of `setNameplates`." Read
            // out: ONE scene cue carrying all four routes (box 1 → slot 3, box 2 empty, host chair →
            // slot 1, reader chair → slot 2), addressed BY ROUTE ID, and re-applying the same
            // placement adds nothing. The plates that follow are the same look resolving, not a
            // second placement.
            ["selecting a look applies its preset and both chairs"] = Sequence(SeatingPreamble, FullLook),

            // INTENT: "a host that declares `hasPreviewBus: false` must never receive
            // `setPreview`/`cut`/`auto` … and a cut must still put the staged source on program."
            // The WinUI shell declares hasPreviewBus TRUE (spec §8), so this is the rich branch:
            // stage the gallery preset — cueing it while rewriting NO routes, which is what an empty
            // route dictionary means — then take with the "cut" transition.
            ["transport honors the host's declared preview bus"] = Sequence(
                SeatingPreamble,
                new[]
                {
                    "SceneExists(gallery-scene)",
                    "CueSceneWithRoutes(gallery-scene, )",
                    "CanTake",
                    "TakeAsync(cut)"
                }),

            // INTENT: "`setGallery` carries the WHOLE cell map in one call … capped at the host's
            // declared `maxGalleryCells`, with `0` meaning a blank cell." Read against spec §8,
            // which does NOT apply setGallery in 7a: the case sends three whole-map calls (the
            // first-tick blank map, the composed map, then one cell blanked) and the correct
            // adapter behaviour is to record all three and speak ONCE. A second status line would
            // be a status-per-tick regression; a route or slot write would be 7a applying a command
            // it has no API for.
            ["the gallery composes from seating and respects the host's cell cap"] = SeatingPreamble,

            // INTENT: "Re-rendering an identical lower third restarts its on-air animation, so
            // `setNameplates`/`setQuestion` must go out on a real change and stay silent otherwise."
            // Read out: plates for the host chair, the reader chair and the filled box; then two
            // silent ticks contributing NOTHING; then — the box cleared — the same look again with
            // its guest route emptied and plates naming only the two chairs. Each plate is two
            // facade calls (display name + lower-third title). The question overlay is null
            // throughout (no question feed is configurable in CONFORMANCE_CONFIG), which is an
            // empty caption, published alongside each plate change.
            ["nameplates and question emit on change and stay silent otherwise"] =
                Sequence(SeatingPreamble, FullLook, EmptyBoxLook),

            // INTENT: "The engine ticks continuously; a host that received work every tick would be
            // rebinding inputs and re-rastering overlays at tick rate, which is the churn class this
            // repo's own shell has a documented fail-fast from." Read out: the setup traffic and
            // NOTHING after it. The trailing three quiet ticks must add zero lines — a golden that
            // grew a tail here would be that churn class arriving through the adapter. (The second
            // setGallery from `gallery.resetFromSlots` adds no status: report-once.)
            ["a tick that changes nothing sends nothing"] = Sequence(SeatingPreamble, EmptyBoxLook)
        };
}
