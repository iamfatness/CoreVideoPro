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
}
