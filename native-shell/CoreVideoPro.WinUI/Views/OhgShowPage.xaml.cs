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

    /// <summary>Same guard for the look picker (<see cref="SyncLookCombo"/>) and the override-role
    /// picker (<see cref="SyncOverrideRoleCombo"/>).</summary>
    private bool _applyingLookSelection;
    private bool _applyingOverrideRoleSelection;

    /// <summary>Same guard for the two <c>ToggleSwitch</c>es. It covers the write
    /// <see cref="OnToggleLoaded"/> makes; the binding's own write is caught by the value
    /// comparison in <see cref="OhgShowPageLogic.ToggleChangeFor"/> (see its remarks).</summary>
    private bool _applyingToggle;

    /// <summary>The view model this page currently has a <c>PropertyChanged</c> subscription on.
    /// Held so the subscription can be moved when the ViewModel property changes and dropped on
    /// unload — a page that outlives its subscription leaks, and one that keeps a stale
    /// subscription re-syncs a combo against a view model nobody is looking at.</summary>
    private OhgShowViewModel? _subscribedShow;

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
            new PropertyMetadata(null, OnViewModelPropertyChanged));

    /// <summary>The workspace assigns <see cref="ViewModel"/> AFTER the page is constructed (and,
    /// depending on tab order, after it has loaded), so the look picker's data source arrives late.
    /// Re-attaching here — rather than only in <c>Loaded</c> — is what makes the picker correct on
    /// the first frame the operator ever sees.
    ///
    /// A DP-changed callback is a FRAMEWORK callback like any XAML event handler — WinUI invokes it
    /// while setting the property, and a throw here fail-fasts the process with no managed stack
    /// exactly as a throwing Click would. It is guarded for that reason, and the guard-coverage test
    /// polices every method wired through <c>DependencyProperty.Register</c> precisely because these
    /// callbacks are invisible in the XAML text where the other handlers are named.</summary>
    private static void OnViewModelPropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is OhgShowPage page) page.Guarded("view model attach", page.AttachShowEvents);
    }

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

    /// <summary>Enables a control only when the wire value it acts on actually exists. Used by
    /// direct-cut: before the first snapshot there IS no preview source, and a cut that sends null
    /// is a refusal the operator has to read instead of a button that was never armed.</summary>
    public static bool EnabledWhenText(string? value) => !string.IsNullOrWhiteSpace(value);

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

    // ── Task 8: program / gallery / GFX helpers ───────────────────────────────────────

    public static string BoxNumberLabel(int box) => $"BOX {box}";

    /// <summary>What a look box shows. An unfilled box reads EMPTY; a box whose seat carries no
    /// resolvable name reads its seat number rather than a blank tile — "we have a seat here but
    /// not a name" is different information from "nothing is here", and the operator needs both.
    /// </summary>
    public static string BoxLabel(int? slot, string? displayName)
    {
        if (slot is not int seat) return "EMPTY";
        return string.IsNullOrWhiteSpace(displayName) ? $"seat {seat}" : displayName!;
    }

    public static string BoxAutomationName(int box) => $"OHG box {box}";

    public static string PreviewBoxAutomationName(int box) => $"OHG preview box {box}";

    public static string ClearBoxAutomationName(int box) => $"OHG clear box {box}";

    public static string GalleryCellNumberLabel(int cell) => $"{cell}";

    /// <summary>A blank gallery cell is a live row (the 4x4 grid never restructures), so it says
    /// BLANK rather than rendering as an empty tile that reads like a layout bug.</summary>
    public static string GalleryCellLabel(bool isBlank, string? displayName)
        => isBlank || string.IsNullOrWhiteSpace(displayName) ? "BLANK" : displayName!;

    public static string GalleryCellAutomationName(int cell) => $"OHG gallery cell {cell}";

    public static string RemoveCellAutomationName(int cell) => $"OHG remove gallery cell {cell}";

    /// <summary>The question card's body. No question is stated ("no question staged"), never an
    /// empty card the operator has to interpret.</summary>
    public static string QuestionTextLabel(string? questionText)
        => string.IsNullOrWhiteSpace(questionText) ? "no question staged" : questionText!;

    /// <summary>A Mukana capability lamp's chip text, e.g. <c>"REGISTRY · AVAILABLE"</c>.</summary>
    public static string LampLabel(string name, string? state)
        => $"{name} · {(string.IsNullOrWhiteSpace(state) ? "UNKNOWN" : state!.ToUpperInvariant())}";

    /// <summary>The lamp's colour. The decision itself is
    /// <see cref="OhgShowPageLogic.LampBrushKey"/> (a pure, unit-tested key) — this only resolves
    /// that key against the theme, which needs a XAML runtime and so cannot be tested.</summary>
    public static Brush LampBrush(string? state)
    {
        var key = OhgShowPageLogic.LampBrushKey(state);
        return Resource(key, key switch
        {
            "StudioLiveBrush" => 0x22C86EU,
            "StudioProgramBrush" => 0xE8A41FU,
            _ => 0x8B949BU,
        });
    }

    /// <summary>Worst-of Mukana health (<c>ok | dormant | failing</c>, ranked by
    /// <c>worstMukanaHealth</c> in the engine's <c>controlState.ts</c>). Failing is AIR RED here
    /// even though the page reserves red for tally elsewhere: a dead data plane silently seats the
    /// wrong names and captions the wrong people, which is an on-air failure, not a hint.</summary>
    public static Brush HealthBrush(string? worst) => (worst ?? "").ToLowerInvariant() switch
    {
        "ok" => Resource("StudioLiveBrush", 0x22C86EU),
        "dormant" => Resource("StudioMutedBrush", 0x8B949BU),
        _ => Resource("StudioAirBrush", 0xE5433FU),
    };

    public static string HealthLabel(string? worst)
        => $"MUKANA {(string.IsNullOrWhiteSpace(worst) ? "UNKNOWN" : worst!.ToUpperInvariant())}";

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

    // ── Task 8 handlers (look picker, toggles, override role, box preview) ────────────

    private void OnPageLoaded(object sender, RoutedEventArgs e) => Guarded("page attach", AttachShowEvents);

    private void OnPageUnloaded(object sender, RoutedEventArgs e) => Guarded("page detach", DetachShowEvents);

    /// <summary>Subscribes to the view model's <c>PropertyChanged</c> so the look picker can be
    /// re-selected when the ENGINE changes the cued look (a look cued from Companion/OSC, or the
    /// engine self-correcting a refused <c>ohg.look.set</c>). Idempotent: re-attaching to the same
    /// view model is a no-op, so Loaded + the DP callback can both call it.</summary>
    private void AttachShowEvents()
    {
        var show = Show;
        if (ReferenceEquals(show, _subscribedShow))
        {
            SyncLookCombo();
            SyncOverrideRoleCombo();
            return;
        }

        DetachShowEvents();
        if (show is null) return;

        _subscribedShow = show;
        show.PropertyChanged += OnShowPropertyChanged;
        SyncLookCombo();
        SyncOverrideRoleCombo();
    }

    private void DetachShowEvents()
    {
        if (_subscribedShow is null) return;
        _subscribedShow.PropertyChanged -= OnShowPropertyChanged;
        _subscribedShow = null;
    }

    private void OnShowPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
        => Guarded("look combo sync", () =>
        {
            if (e.PropertyName == nameof(OhgShowViewModel.SelectedLookId)) SyncLookCombo();
            else if (e.PropertyName == nameof(OhgShowViewModel.OverrideRole)) SyncOverrideRoleCombo();
        });

    private void OnLookComboLoaded(object sender, RoutedEventArgs e)
        => Guarded("look combo sync", SyncLookCombo);

    /// <summary>Writes the look picker's items and selection — the ONLY writer of either, exactly
    /// like <see cref="SyncRoleCombo"/>. The change handler is detached across the write and the
    /// re-entrancy flag is set, so the engine never sees this page's own selection echoed back as
    /// an operator cue.</summary>
    private void SyncLookCombo()
    {
        if (OhgLookCombo is not { } combo) return;

        try
        {
            if (Show is not { } show) return;

            _applyingLookSelection = true;
            combo.SelectionChanged -= OnLookSelectionChanged;
            combo.DisplayMemberPath = "Label";
            combo.ItemsSource = show.Looks;
            combo.SelectedItem = show.Looks.FirstOrDefault(look => look.Id == show.SelectedLookId);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: look selection skipped ({ex.GetType().Name}: {ex.Message})");
        }
        finally
        {
            _applyingLookSelection = false;
            combo.SelectionChanged -= OnLookSelectionChanged;
            combo.SelectionChanged += OnLookSelectionChanged;
        }
    }

    private void OnLookSelectionChanged(object sender, SelectionChangedEventArgs e)
        => Guarded("look change", () =>
        {
            if (_applyingLookSelection) return;
            if (sender is not ComboBox combo || Show is not { } show) return;

            var picked = (combo.SelectedItem as OhgLookOption)?.Id;
            if (OhgShowPageLogic.LookChangeFor(show.SelectedLookId, picked) is not { } lookId) return;

            show.SetLookCommand.Execute(lookId);
        });

    private void OnOverrideRoleComboLoaded(object sender, RoutedEventArgs e)
        => Guarded("override role sync", SyncOverrideRoleCombo);

    private void SyncOverrideRoleCombo()
    {
        if (OhgOverrideRoleCombo is not { } combo) return;

        try
        {
            if (Show is not { } show) return;

            _applyingOverrideRoleSelection = true;
            combo.SelectionChanged -= OnOverrideRoleSelectionChanged;

            // Roles are a fixed list, so re-assigning ItemsSource on every re-sync would drop and
            // rebuild the items (and any open popup) for nothing.
            if (combo.ItemsSource is null) combo.ItemsSource = show.Roles;
            combo.SelectedItem = show.Roles.Contains(show.OverrideRole) ? show.OverrideRole : null;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: override role selection skipped ({ex.GetType().Name}: {ex.Message})");
        }
        finally
        {
            _applyingOverrideRoleSelection = false;
            combo.SelectionChanged -= OnOverrideRoleSelectionChanged;
            combo.SelectionChanged += OnOverrideRoleSelectionChanged;
        }
    }

    /// <summary>The override role is EDITOR state, not an engine action — it is read by
    /// <c>OverrideSetCommand</c> when the operator commits the form. Nothing is sent here.</summary>
    private void OnOverrideRoleSelectionChanged(object sender, SelectionChangedEventArgs e)
        => Guarded("override role change", () =>
        {
            if (_applyingOverrideRoleSelection) return;
            if (sender is not ComboBox combo || Show is not { } show) return;
            if (combo.SelectedItem is not string role || string.IsNullOrWhiteSpace(role)) return;

            show.OverrideRole = role;
        });

    /// <summary>Belt-and-braces initial state for a <c>ToggleSwitch</c>: if the OneWay binding has
    /// not landed by the time the switch realizes, this writes the view model's value — under the
    /// re-entrancy flag, because writing <c>IsOn</c> raises <c>Toggled</c>.</summary>
    private void OnToggleLoaded(object sender, RoutedEventArgs e) => Guarded("toggle sync", () =>
    {
        if (sender is not ToggleSwitch toggle || Show is not { } show) return;

        var desired = toggle.Tag as string switch
        {
            "asFollow" => show.AsFollow,
            "smartGallery" => show.SmartGallery,
            _ => (bool?)null,
        };

        if (desired is not bool value || toggle.IsOn == value) return;

        _applyingToggle = true;
        try
        {
            toggle.IsOn = value;
        }
        finally
        {
            _applyingToggle = false;
        }
    });

    private void OnAsFollowToggled(object sender, RoutedEventArgs e) => Guarded("as-follow toggle", () =>
    {
        if (_applyingToggle) return;
        if (sender is not ToggleSwitch toggle || Show is not { } show) return;
        if (OhgShowPageLogic.ToggleChangeFor(show.AsFollow, toggle.IsOn) is not bool on) return;

        show.SetAsFollowCommand.Execute(on);
    });

    private void OnSmartGalleryToggled(object sender, RoutedEventArgs e) => Guarded("smart gallery toggle", () =>
    {
        if (_applyingToggle) return;
        if (sender is not ToggleSwitch toggle || Show is not { } show) return;
        if (OhgShowPageLogic.ToggleChangeFor(show.SmartGallery, toggle.IsOn) is not bool on) return;

        show.SetSmartGalleryCommand.Execute(on);
    });

    /// <summary>Cues the seat sitting in a look box. A click handler rather than a bound command
    /// because the box's slot is nullable and <c>PreviewSlotCommand</c> takes an <c>int</c> — a
    /// null CommandParameter reaching a <c>RelayCommand&lt;int&gt;</c> throws INSIDE the flyout's
    /// invoke, which is the fail-fast shape this whole page is written to avoid.</summary>
    private void OnPreviewBoxSlotClick(object sender, RoutedEventArgs e) => Guarded("preview box slot", () =>
    {
        if (RowFrom<OhgBoxViewModel>(sender) is not { } row || Show is not { } show) return;
        if (row.Slot is not int slot)
        {
            show.LastActionStatus = "That box is empty";
            return;
        }

        show.PreviewSlotCommand.Execute(slot);
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
