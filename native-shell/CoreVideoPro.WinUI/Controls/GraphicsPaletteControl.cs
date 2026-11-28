using System.Globalization;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics → Design → Palette (graphics styles slice 1). Edits the show palette stored
/// in the graphics document. Saving recolours Cue and Preview at once
/// (<see cref="GraphicsPlayoutService.RefreshPaletteAsync"/>); Program changes only through the
/// one "Update on air" button, whose count comes from native Program revisions, never from
/// local intent. Rows are built once; refreshes update text and swatches in place (no
/// collection churn, docs/reference/winui-crash-class-0xc000027b.md). Uses the existing WinUI
/// ColorPicker; the shared CvColorField replaces it in slice 2.</summary>
public sealed class GraphicsPaletteControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsPaletteControl), new PropertyMetadata(null, (d, _) => ((GraphicsPaletteControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }

    private static readonly IReadOnlyDictionary<string, string> Labels = new Dictionary<string, string>(StringComparer.Ordinal) {
        [GraphicsPalette.Primary] = "Primary (main plate)", [GraphicsPalette.Secondary] = "Secondary (title bar)", [GraphicsPalette.Accent] = "Accent (strips, tabs)",
        [GraphicsPalette.Surface] = "Surface (neutral plate)", [GraphicsPalette.OnPrimary] = "Text on primary", [GraphicsPalette.OnPrimaryMuted] = "Second line on primary",
        [GraphicsPalette.OnSecondary] = "Text on secondary", [GraphicsPalette.OnAccent] = "Text on accent", [GraphicsPalette.TeamA] = "Team A (sports)", [GraphicsPalette.TeamB] = "Team B (sports)" };

    private sealed record Row(string Token, Border Chip, TextBox Hex, CheckBox? Auto, Button? Clear, ColorPicker Picker);
    private readonly List<Row> _rows = [];
    private readonly TextBlock _status = new() { FontSize = 11, TextWrapping = TextWrapping.WrapWholeWords };
    private readonly Button _updateOnAir;
    private StudioViewModel? _attached;
    private bool _loading, _saving;

    public GraphicsPaletteControl()
    {
        var root = new StackPanel { Spacing = 8, Padding = new Thickness(12, 10, 12, 12), Background = StudioBrushes.Panel,
            BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };
        root.Children.Add(new TextBlock { Text = "Show colours. Every graphic that uses a palette colour follows it. Cue and Preview change as you edit; Program changes only when you press Update on air.",
            FontSize = 11, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.WrapWholeWords });
        _updateOnAir = StudioBrushes.AccentButton("Update on air"); _updateOnAir.Visibility = Visibility.Collapsed;
        _updateOnAir.Click += async (_, _) => await UpdateOnAirAsync();
        root.Children.Add(_updateOnAir);
        var grid = new Grid { ColumnSpacing = 16, RowSpacing = 6 };
        grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        for (var i = 0; i < GraphicsPalette.Tokens.Count; i++) {
            var cell = BuildRow(GraphicsPalette.Tokens[i]);
            if (grid.RowDefinitions.Count <= i / 2) grid.RowDefinitions.Add(new() { Height = GridLength.Auto });
            Grid.SetRow(cell, i / 2); Grid.SetColumn(cell, i % 2); grid.Children.Add(cell);
        }
        root.Children.Add(grid);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var saveDefault = StudioBrushes.GhostButton("Save as app default"); saveDefault.Click += async (_, _) => await SaveAppDefaultAsync();
        ToolTipService.SetToolTip(saveDefault, "New shows start from this palette. The open show is unchanged.");
        var reset = StudioBrushes.GhostButton("Use app default"); reset.Click += async (_, _) => { if (Library is { } l) await SaveAsync(l.AppDefaultPalette); };
        ToolTipService.SetToolTip(reset, "Replace this show's palette with the app default.");
        actions.Children.Add(saveDefault); actions.Children.Add(reset);
        root.Children.Add(actions);
        root.Children.Add(_status);
        Content = root;
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
    }

    private GraphicsWorkspaceLibrary? Library => _attached?.GraphicsLibrary;

    private FrameworkElement BuildRow(string token)
    {
        var panel = new Grid { ColumnSpacing = 6 };
        foreach (var width in new[] { new GridLength(150), GridLength.Auto, new GridLength(1, GridUnitType.Star), GridLength.Auto })
            panel.ColumnDefinitions.Add(new() { Width = width });
        panel.Children.Add(new TextBlock { Text = Labels[token], FontSize = 12, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis });
        var chip = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(4), BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), Background = StudioBrushes.Field };
        var picker = new ColorPicker { IsAlphaEnabled = true, IsAlphaSliderVisible = true, IsAlphaTextInputVisible = true, IsHexInputVisible = true, IsMoreButtonVisible = true };
        var flyout = new Flyout { Content = picker, Placement = FlyoutPlacementMode.Bottom };
        var swatch = StudioBrushes.GhostButton(""); swatch.Content = chip; swatch.Padding = new Thickness(4); swatch.Flyout = flyout;
        AutomationName(swatch, "Pick " + Labels[token]);
        Grid.SetColumn(swatch, 1); panel.Children.Add(swatch);
        var hex = new TextBox { PlaceholderText = "#RRGGBBAA", FontFamily = StudioBrushes.Mono, FontSize = 12, MaxLength = 9, VerticalAlignment = VerticalAlignment.Center };
        AutomationName(hex, Labels[token] + " hex");
        Grid.SetColumn(hex, 2); panel.Children.Add(hex);
        CheckBox? auto = null; Button? clear = null;
        if (GraphicsPalette.OnColourPlates.ContainsKey(token)) {
            auto = new CheckBox { Content = "Auto", MinWidth = 0, FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
            ToolTipService.SetToolTip(auto, "White or near-black, whichever reads better on its plate");
            auto.Click += async (_, _) => { if (!_loading) await CommitAsync(token, auto.IsChecked == true ? GraphicsPalette.Auto : Resolved(token) ?? GraphicsPalette.White); };
            Grid.SetColumn(auto, 3); panel.Children.Add(auto);
        } else if (!GraphicsPalette.Required.Contains(token, StringComparer.Ordinal)) {
            clear = StudioBrushes.ChipButton("Clear"); clear.Click += async (_, _) => await CommitAsync(token, "");
            ToolTipService.SetToolTip(clear, "Unset: templates that use it fall back to their own default");
            Grid.SetColumn(clear, 3); panel.Children.Add(clear);
        }
        hex.KeyDown += async (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) await CommitHexAsync(token, hex); };
        hex.LostFocus += async (_, _) => await CommitHexAsync(token, hex);
        // Dragging in the picker fires ColorChanged continuously; one save when the flyout closes.
        flyout.Closed += async (_, _) => {
            if (_loading) return;
            var c = picker.Color; var value = $"#{c.R:X2}{c.G:X2}{c.B:X2}{c.A:X2}";
            if (!string.Equals(value, Resolved(token), StringComparison.OrdinalIgnoreCase)) await CommitAsync(token, value);
        };
        _rows.Add(new(token, chip, hex, auto, clear, picker));
        return panel;
    }
    private static void AutomationName(DependencyObject element, string name) => Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(element, name);

    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach(); _attached = Studio;
        _attached.PropertyChanged += OnStudioProperty; _attached.GraphicsLibrary.Changed += OnLibraryChanged;
        _ = _attached.GraphicsLibrary.LoadAsync();
        Refresh();
    }
    private void Detach()
    {
        if (_attached is not null) { _attached.PropertyChanged -= OnStudioProperty; _attached.GraphicsLibrary.Changed -= OnLibraryChanged; }
        _attached = null;
    }
    private void OnLibraryChanged(object? sender, EventArgs e) => DispatcherQueue?.TryEnqueue(Refresh);
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.GraphicsStrip)) RefreshOnAir();
    }

    private string? Resolved(string token) => Library?.EffectivePalette.Resolve().GetValueOrDefault(token);

    private void Refresh()
    {
        if (Library is not { } library) return;
        var palette = library.EffectivePalette; var resolved = palette.Resolve();
        _loading = true;
        try {
            foreach (var row in _rows) {
                var stored = palette.Colors.GetValueOrDefault(row.Token, "");
                var value = resolved.GetValueOrDefault(row.Token, "");
                if (row.Hex.FocusState == FocusState.Unfocused) row.Hex.Text = value;
                row.Hex.IsEnabled = stored != GraphicsPalette.Auto;
                if (row.Auto is not null) row.Auto.IsChecked = stored == GraphicsPalette.Auto;
                if (row.Clear is not null) row.Clear.Visibility = stored.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
                if (TryColor(value, out var color)) { row.Chip.Background = new SolidColorBrush(color); row.Picker.Color = color; }
                else row.Chip.Background = StudioBrushes.Field;
                // Non-blocking contrast note (spec 4.2 states 4.5:1 for on-air text); the full checks arrive with CvColorField.
                if (GraphicsPalette.OnColourPlates.TryGetValue(row.Token, out var plate) && value.Length == 9 && resolved.TryGetValue(plate, out var plateValue)) {
                    var ratio = GraphicsPalette.Contrast(value, plateValue);
                    ToolTipService.SetToolTip(row.Hex, ratio < 4.5 ? $"Contrast {ratio.ToString("0.0", CultureInfo.InvariantCulture)}:1 on its plate — below 4.5:1 on air"
                        : $"Contrast {ratio.ToString("0.0", CultureInfo.InvariantCulture)}:1 on its plate");
                }
            }
        } finally { _loading = false; }
        if (!_saving && _status.Text.Length == 0 && library.PaletteMigrated)
            _status.Text = "This show was made before the palette. It now uses the app default, and each existing graphic keeps its own colours until you choose Use show palette on it.";
        RefreshOnAir();
    }

    private void RefreshOnAir()
    {
        if (_attached is null) return;
        var stale = _attached.GraphicsPlayout.PaletteStaleOnAir().Count;
        _updateOnAir.Visibility = stale > 0 ? Visibility.Visible : Visibility.Collapsed;
        _updateOnAir.Content = stale == 1 ? "1 graphic on air uses an older palette — Update on air" : $"{stale} graphics on air use an older palette — Update on air";
    }

    private async Task CommitHexAsync(string token, TextBox box)
    {
        if (_loading || Library is null) return;
        var text = box.Text.Trim();
        if (string.Equals(text, Resolved(token) ?? "", StringComparison.OrdinalIgnoreCase)) return;
        if (text.Length == 0 && !GraphicsPalette.Required.Contains(token, StringComparer.Ordinal)) { await CommitAsync(token, ""); return; }
        if (!GraphicsPalette.TryNormalize(text, out var value)) { _status.Text = $"{Labels[token]}: use #RGB, #RRGGBB or #RRGGBBAA."; return; }
        await CommitAsync(token, value);
    }

    private Task CommitAsync(string token, string value) => Library is { } library ? SaveAsync(library.EffectivePalette.With(token, value)) : Task.CompletedTask;

    private async Task SaveAsync(GraphicsPalette palette)
    {
        if (_attached is not { } studio || _saving) return;
        _saving = true;
        try {
            if (!await studio.GraphicsLibrary.SavePaletteAsync(palette)) { _status.Text = studio.GraphicsLibrary.Status; return; }
            // Only cue copies are refreshed; Program waits for Update on air.
            var refreshed = await studio.GraphicsPlayout.RefreshPaletteAsync();
            _status.Text = refreshed.Accepted ? studio.GraphicsLibrary.Status + " " + refreshed.Reason : refreshed.Reason;
        } finally { _saving = false; Refresh(); }
    }

    private async Task SaveAppDefaultAsync()
    {
        if (Library is not { } library) return;
        await library.SaveAppDefaultPaletteAsync(library.EffectivePalette);
        _status.Text = library.Status;
    }

    private async Task UpdateOnAirAsync()
    {
        if (_attached is not { } studio) return;
        _updateOnAir.IsEnabled = false;
        try { var result = await studio.GraphicsPlayout.UpdateOnAirAsync(); _status.Text = result.Reason; }
        finally { _updateOnAir.IsEnabled = true; RefreshOnAir(); }
    }

    private static bool TryColor(string value, out Windows.UI.Color color)
    {
        color = default;
        if (!GraphicsPalette.IsColor(value)) return false;
        byte B(int i) => byte.Parse(value.AsSpan(i, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        color = Windows.UI.Color.FromArgb(B(7), B(1), B(3), B(5)); return true;
    }
}
