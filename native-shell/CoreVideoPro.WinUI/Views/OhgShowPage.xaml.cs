using System;
using System.Linq;
using CoreVideoPro.WinUI;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// The OHG Show tab (Plan 7b Task 7) — status strip + panelist board. Mirrors
/// <see cref="AutomationPage"/>: a <see cref="UserControl"/> with a single <c>ViewModel</c>
/// dependency property, so the workspace hosts it with one x:Bind and the page owns no state.
///
/// Everything on this page that is NOT a plain value binding lives here as a PURE STATIC, called
/// from XAML through x:Bind function binding. That is deliberate: an <c>IValueConverter</c> is an
/// untestable instance in a resource dictionary, whereas these are ordinary methods a unit test can
/// call. The only instance code is the three event handlers, and they exist because of two house
/// rules from CLAUDE.md:
///
/// <list type="number">
/// <item><b><c>Selector.SelectedValue</c> is never driven by x:Bind inside an
/// <see cref="ItemsRepeater"/>.</b> A recycled container can be re-bound while its ItemsSource is
/// still resolving; selecting a value the ComboBox does not yet contain threw COMException
/// 0x80004005 out of <c>Selector.set_SelectedValue</c> and killed the app. Selection is applied
/// here, with the change handler detached, inside a try/catch — a wrong dropdown is cosmetic, a
/// crash ends the show.</item>
/// <item><b>The row's DataContext is null inside an ItemsRepeater template.</b> Every template
/// carries <c>Tag="{x:Bind}"</c> and the handlers read the row from <c>Tag</c>, falling back to
/// DataContext only for completeness.</item>
/// </list>
/// </summary>
public sealed partial class OhgShowPage : UserControl
{
    /// <summary>Re-entrancy guard: the programmatic selection in <see cref="SyncRoleCombo"/> must
    /// not be echoed back to the engine as an operator role change.</summary>
    private bool _applyingRoleSelection;

    public OhgShowPage()
    {
        InitializeComponent();
    }

    public StudioViewModel? ViewModel
    {
        get => (StudioViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(
            nameof(ViewModel),
            typeof(StudioViewModel),
            typeof(OhgShowPage),
            new PropertyMetadata(null));

    // ── pure helpers (x:Bind function bindings) ───────────────────────────────────────

    /// <summary>The setup surface shows exactly when OHG is not configured.</summary>
    public static Visibility VisibleWhenNull(object? value)
        => value is null ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>The workspace shows exactly when it has a view model to render.</summary>
    public static Visibility VisibleWhenNotNull(object? value)
        => value is null ? Visibility.Collapsed : Visibility.Visible;

    public static Visibility VisibleWhenTrue(bool value)
        => value ? Visibility.Visible : Visibility.Collapsed;

    /// <summary>Non-empty text is a real message; empty text is not a blank row.</summary>
    public static Visibility VisibleWhenText(string? value)
        => string.IsNullOrWhiteSpace(value) ? Visibility.Collapsed : Visibility.Visible;

    /// <summary>The seat's tally border: air red when the seat is on program, otherwise the
    /// ordinary panel line. This is the ONE piece of tally on the page, so it is a function of the
    /// engine's <c>onAir</c> flag alone — never of selection, which is page state.</summary>
    public static Brush OnAirBrush(bool onAir)
        => Resource(onAir ? "StudioAirBrush" : "StudioBorderBrush", onAir ? 0xE5433FU : 0x2A343CU);

    /// <summary>Selection is a page affordance and reads as accent, never as tally.</summary>
    public static Brush SelectionBrush(bool isSelected)
        => Resource(isSelected ? "StudioAccentBrush" : "StudioBorderBrush", isSelected ? 0x22C86EU : 0x2A343CU);

    /// <summary>Engine health, colour-coded the way the rest of the console codes state: running is
    /// live green, a transient (starting/recovering) is program amber, and stopped or failed is air
    /// red — a stopped show engine is a failure to notice, not a neutral idle.</summary>
    public static Brush EngineStateBrush(string? state) => (state ?? "").ToLowerInvariant() switch
    {
        "running" => Resource("StudioLiveBrush", 0x22C86EU),
        "starting" or "recovering" => Resource("StudioProgramBrush", 0xE8A41FU),
        _ => Resource("StudioAirBrush", 0xE5433FU),
    };

    public static string EngineStateLabel(string? state)
        => string.IsNullOrWhiteSpace(state) ? "UNKNOWN" : state.ToUpperInvariant();

    public static string ShadowLabel(bool isShadowMode)
        => isShadowMode ? "SHADOW — host commands are logged, never applied" : "DRIVING HOST";

    public static string SeatLabel(bool isOccupied, string? displayName)
        => isOccupied && !string.IsNullOrWhiteSpace(displayName) ? displayName! : "EMPTY";

    public static string SeatNumberLabel(int slot) => slot.ToString();

    public static string PinLabel(string? pin) => string.IsNullOrWhiteSpace(pin) ? "NO PIN" : pin!;

    public static string GlyphLabel(bool videoOn, bool audioOn, bool handRaised, bool online)
    {
        var glyphs = string.Concat(
            online ? "●" : "○",
            videoOn ? " CAM" : "",
            audioOn ? " MIC" : "",
            handRaised ? " HAND" : "");
        return glyphs;
    }

    // Automation names. Bound (not literal) wherever the control names a specific row, so a screen
    // reader — and our own UIA tooling — can tell "OHG slot 3" from "OHG slot 4".
    public static string PanelistAutomationName(string? displayName)
        => $"OHG panelist {Fallback(displayName)}";

    public static string RoleAutomationName(string? displayName)
        => $"OHG role for {Fallback(displayName)}";

    public static string SlotAutomationName(int slot) => $"OHG slot {slot}";

    public static string RemoveSlotAutomationName(int slot) => $"OHG remove slot {slot}";

    private static string Fallback(string? name) => string.IsNullOrWhiteSpace(name) ? "unnamed" : name!;

    private static Brush Resource(string key, uint rgb)
    {
        if (Application.Current?.Resources is { } resources &&
            resources.TryGetValue(key, out var value) &&
            value is Brush brush)
        {
            return brush;
        }

        return new SolidColorBrush(Windows.UI.Color.FromArgb(
            255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb));
    }

    // ── selection (code-behind, never x:Bind) ─────────────────────────────────────────

    private OhgShowViewModel? Show => ViewModel?.OhgShow;

    private static T? RowFrom<T>(object sender) where T : class
    {
        if (sender is not FrameworkElement element) return null;
        return element.Tag as T ?? element.DataContext as T;
    }

    /// <summary>
    /// The ONE shape every UI callback on this page uses. CLAUDE.md, live-QA day: a throwing
    /// DispatcherQueue/UI callback fail-fasts the process with NO managed stack — three live
    /// crashes decoded to an ordinary NRE inside a queued callback. A handler here can genuinely
    /// throw: <c>SetRoleCommand</c> is an AsyncRelayCommand, so <c>Execute</c> runs synchronously
    /// to the first await and rethrows a faulted task onto the UI thread, and
    /// <see cref="FindDescendant{T}"/> walks a live visual tree that can be torn down underneath
    /// it. A misbehaving dropdown is cosmetic; a fail-fast ends the show.
    /// </summary>
    private void Guarded(string what, Action body)
    {
        try
        {
            body();
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: {what} skipped ({ex.GetType().Name}: {ex.Message})");
        }
    }

    private void OnPanelistClick(object sender, RoutedEventArgs e) => Guarded("panelist select", () =>
    {
        if (RowFrom<OhgPanelistRowViewModel>(sender) is not { } row || Show is not { } show) return;
        show.SelectedParticipantId = row.ParticipantId;
    });

    private void OnSeatClick(object sender, RoutedEventArgs e) => Guarded("seat select", () =>
    {
        // The seat button's Command already assigns the selected panelist; selecting the seat too
        // is what makes the seat's own flyout (Remove) address the seat the operator just touched.
        if (RowFrom<OhgSlotRowViewModel>(sender) is not { } row || Show is not { } show) return;
        show.SelectedSlot = row.Slot;
    });

    private void OnRoleComboLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is ComboBox combo) SyncRoleCombo(combo);
    }

    private void OnPanelistElementPrepared(ItemsRepeater sender, ItemsRepeaterElementPreparedEventArgs args)
        => Guarded("role combo realize", () =>
        {
            if (args.Element is FrameworkElement root &&
                FindDescendant<ComboBox>(root, "OhgRoleCombo") is { } combo)
            {
                SyncRoleCombo(combo);
            }
        });

    /// <summary>Applies the row's role to the ComboBox with the change handler DETACHED and only
    /// after the items exist, never assigning a value the list does not contain. See the class
    /// remarks for the crash this shape exists to avoid. The ItemsSource is assigned HERE and
    /// nowhere else — the template deliberately carries no ItemsSource binding, so there is exactly
    /// one writer and no race between an ElementName binding resolving and this selection.</summary>
    private void SyncRoleCombo(ComboBox combo)
    {
        try
        {
            if (RowFrom<OhgPanelistRowViewModel>(combo) is not { } row || Show is not { } show) return;

            _applyingRoleSelection = true;
            combo.SelectionChanged -= OnRoleSelectionChanged;
            combo.ItemsSource = show.Roles;
            combo.SelectedItem = show.Roles.Contains(row.Role) ? row.Role : null;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: role selection skipped ({ex.GetType().Name}: {ex.Message})");
        }
        finally
        {
            _applyingRoleSelection = false;
            combo.SelectionChanged -= OnRoleSelectionChanged;
            combo.SelectionChanged += OnRoleSelectionChanged;
        }
    }

    private void OnRoleSelectionChanged(object sender, SelectionChangedEventArgs e)
        => Guarded("role change", () =>
        {
            if (_applyingRoleSelection) return;
            if (sender is not ComboBox combo ||
                RowFrom<OhgPanelistRowViewModel>(combo) is not { } row ||
                Show is not { } show)
            {
                return;
            }

            if (OhgShowPageLogic.RoleChangeFor(row.Role, row.Pin, combo.SelectedItem as string) is not { } change)
            {
                return;
            }

            show.SetRoleCommand.Execute(change);
        });

    private static T? FindDescendant<T>(DependencyObject root, string name) where T : FrameworkElement
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var index = 0; index < count; index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T typed && typed.Name == name) return typed;
            if (FindDescendant<T>(child, name) is { } found) return found;
        }

        return null;
    }
}
