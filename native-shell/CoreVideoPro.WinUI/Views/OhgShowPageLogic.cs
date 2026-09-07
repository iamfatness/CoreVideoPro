using System;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// The decisions <see cref="OhgShowPage"/>'s event handlers make, as pure functions.
///
/// They live in their own type rather than as statics on the page because
/// <see cref="OhgShowPage"/> holds a <c>DependencyProperty.Register</c> static field: touching any
/// static member of a <c>UserControl</c> can run that initializer, which throws outside a XAML
/// runtime. A separate class is reachable from a unit test.
///
/// This matters because the handlers themselves have no test seam — every one of them is now a
/// try/catch wrapper (a throwing UI callback fail-fasts the process with no managed log; see
/// CLAUDE.md's <c>UiDispatch</c> rule), and a wrapper is verified by reading. The decision inside it
/// is verified by a test.
/// </summary>
internal static class OhgShowPageLogic
{
    /// <summary>What <c>AttachShowEvents</c> has to do when the page's current show view model is
    /// <paramref name="current"/> and it is presently subscribed to <paramref name="previous"/>.
    ///
    /// <para>This exists because of the rebuild path added in Plan 7b Task 10: saving a show config
    /// REPLACES <c>StudioViewModel.OhgShow</c> with a fresh view model and disposes the old one. A
    /// page that only re-attached on Loaded and the DP-changed callback (neither of which fires for
    /// a visibility-hosted page) would keep a PropertyChanged subscription on a DISPOSED view model
    /// and keep the look ComboBox's ItemsSource pointing at the OLD view model's Looks - so picking
    /// a look would send an id from the old config to the new engine.</para>
    ///
    /// <para><c>Resync</c> is true even when nothing changed: Loaded and the DP callback both call
    /// in, and re-selecting the combos against the same view model is cheap and idempotent.</para>
    /// </summary>
    internal static (bool Detach, bool Attach, bool Resync) ShowSubscriptionChange(
        object? previous, object? current)
    {
        if (ReferenceEquals(previous, current))
        {
            return (Detach: false, Attach: false, Resync: true);
        }

        return (Detach: previous is not null, Attach: current is not null, Resync: true);
    }

    /// <summary>
    /// The argument for <c>ohg.panelist.role.set</c> implied by an operator picking
    /// <paramref name="picked"/> in a row's role ComboBox — or <c>null</c> when there is nothing to
    /// send.
    ///
    /// Nothing is sent when the picked value is absent/blank (a ComboBox mid-rebind reports no
    /// selection) or when it already equals the row's role. That second case is what makes the
    /// programmatic selection in <c>SyncRoleCombo</c> harmless even if the re-entrancy flag were
    /// ever bypassed: re-selecting the value the row already has is not an operator edit.
    ///
    /// A missing PIN is deliberately NOT filtered here. The view model refuses it with a specific
    /// message ("Panelist has no PIN; set a Mukana override instead"), which the operator needs to
    /// see — swallowing it in the page would make the dropdown look broken instead.
    /// </summary>
    internal static (string Pin, string Role)? RoleChangeFor(string? currentRole, string? pin, string? picked)
    {
        if (string.IsNullOrWhiteSpace(picked)) return null;
        if (string.Equals(picked, currentRole, StringComparison.Ordinal)) return null;
        return (pin ?? string.Empty, picked!);
    }

    /// <summary>
    /// The look id implied by an operator picking <paramref name="picked"/> in the look ComboBox —
    /// or <c>null</c> when there is nothing to cue.
    ///
    /// Same two refusals as <see cref="RoleChangeFor"/>, for the same reasons: a ComboBox whose
    /// ItemsSource is mid-assignment reports no selection, and re-selecting the look already cued
    /// is not an operator edit. That second refusal is what makes the page's own re-sync (the look
    /// picker is re-selected from the snapshot after every <c>SelectedLookId</c> change) harmless
    /// even if the re-entrancy flag were ever bypassed — otherwise every snapshot would re-issue
    /// <c>ohg.look.set</c> at snapshot rate, i.e. re-cue the show engine's look on a cadence.
    /// </summary>
    internal static string? LookChangeFor(string? currentLookId, string? picked)
    {
        if (string.IsNullOrWhiteSpace(picked)) return null;
        if (string.Equals(picked, currentLookId, StringComparison.Ordinal)) return null;
        return picked;
    }

    /// <summary>
    /// The value to send for a <c>ToggleSwitch</c> that just reported <paramref name="isOn"/>, or
    /// <c>null</c> when the switch merely caught up with the engine.
    ///
    /// Both OHG toggles (AS-follow, Smart gallery) bind <c>IsOn</c> OneWay from the snapshot, and a
    /// binding-driven write raises <c>Toggled</c> exactly like a finger does — WinUI gives the
    /// handler no way to tell them apart. The page carries a re-entrancy flag for the writes it
    /// makes itself, but the binding's write happens outside any code the page runs, so the flag
    /// alone cannot cover it. This comparison can: a switch that now equals the engine's own state
    /// has nothing to send. Without it, every snapshot that flips the engine's value would bounce
    /// straight back as an operator command.
    /// </summary>
    internal static bool? ToggleChangeFor(bool current, bool isOn)
        => isOn == current ? null : isOn;

    /// <summary>
    /// The brush resource key a Mukana capability lamp shows for <paramref name="state"/> (the
    /// engine's <c>available | unavailable | disabled</c>, see
    /// <c>show-engine/src/capabilities.ts</c>). The KEY is returned rather than a
    /// <c>Brush</c> so this decision is testable — resolving a key against
    /// <c>Application.Current.Resources</c> needs a XAML runtime.
    ///
    /// <list type="bullet">
    /// <item><c>available</c> → live green: the endpoint answered.</item>
    /// <item><c>disabled</c> → program amber: Mukana is not configured for this capability, so a
    /// feature the operator may be expecting is deliberately off. Amber is the console's "read
    /// this" colour (air red is reserved for tally).</item>
    /// <item>anything else, including the projection's <c>unavailable</c> default for a missing or
    /// malformed capability node → muted: we do not know that it works, and an unknown is not a
    /// claim. Unrecognized future states land here rather than reading as healthy.</item>
    /// </list>
    /// </summary>
    internal static string LampBrushKey(string? state) => (state ?? "").ToLowerInvariant() switch
    {
        "available" => "StudioLiveBrush",
        "disabled" => "StudioProgramBrush",
        _ => "StudioMutedBrush",
    };
}
