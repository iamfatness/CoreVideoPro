using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>The graphics On Air strip (issue #1076): ON PROGRAM chips (Play Out), ON PREVIEW
/// chips (Remove), the armed-automation chip (Disarm), Clear all and Clear Preview. Bound
/// ONLY to <see cref="StudioViewModel.GraphicsStrip"/>, which is native instance truth; a
/// button press never changes a chip here, only the next native snapshot does.</summary>
public sealed class GraphicsOnAirStripControl : UserControl
{
    public static readonly DependencyProperty StudioProperty = DependencyProperty.Register(nameof(Studio), typeof(StudioViewModel),
        typeof(GraphicsOnAirStripControl), new PropertyMetadata(null, (d, _) => ((GraphicsOnAirStripControl)d).Attach()));
    public StudioViewModel? Studio { get => (StudioViewModel?)GetValue(StudioProperty); set => SetValue(StudioProperty, value); }

    private StudioViewModel? _attached;
    private readonly StackPanel _program = new() { Orientation = Orientation.Horizontal, Spacing = 6 }, _preview = new() { Orientation = Orientation.Horizontal, Spacing = 6 };
    private readonly StackPanel _auto = new() { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _programEmpty = Muted("No graphics on air"), _previewEmpty = Muted("Nothing staged for the next Take"), _status = Muted("");
    private readonly Button _playOutAll, _clearPreview;
    private string _signature = "";

    public GraphicsOnAirStripControl()
    {
        var root = new Grid { ColumnSpacing = 12, Padding = new Thickness(10, 6, 10, 6), MinHeight = 44, Background = StudioBrushes.Panel,
            BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };
        root.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        root.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        root.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        root.Children.Add(Section("ON AIR", StudioBrushes.Air, _program, _programEmpty));
        var preview = Section("CUED", StudioBrushes.Program, _preview, _previewEmpty); Grid.SetColumn(preview, 1); root.Children.Add(preview);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        actions.Children.Add(_auto);
        _playOutAll = StudioBrushes.GhostButton("Clear all"); _playOutAll.Click += async (_, _) => await Invoke(GraphicsPlayoutActions.PlayOutAll);
        _clearPreview = StudioBrushes.GhostButton("Clear cued"); _clearPreview.Click += async (_, _) => await Invoke(GraphicsPlayoutActions.PreviewClear);
        ToolTipService.SetToolTip(_playOutAll, "Hide every graphic on Program (F12 with Shift)");
        ToolTipService.SetToolTip(_clearPreview, "Clear every cued graphic; nothing rides the next Take");
        actions.Children.Add(_playOutAll); actions.Children.Add(_clearPreview);
        _status.MaxWidth = 220; _status.TextTrimming = TextTrimming.CharacterEllipsis; _status.VerticalAlignment = VerticalAlignment.Center; actions.Children.Add(_status);
        Grid.SetColumn(actions, 2); root.Children.Add(actions);
        Content = root;
        Loaded += (_, _) => Attach(); Unloaded += (_, _) => Detach();
    }

    private static FrameworkElement Section(string title, Brush accent, StackPanel chips, TextBlock empty)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        var label = new TextBlock { Text = title, FontSize = 10, CharacterSpacing = 160, Foreground = accent, FontFamily = StudioBrushes.Mono, VerticalAlignment = VerticalAlignment.Center, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold };
        panel.Children.Add(label);
        var scroll = new ScrollViewer { Content = chips, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, VerticalScrollBarVisibility = ScrollBarVisibility.Disabled, VerticalScrollMode = ScrollMode.Disabled, HorizontalScrollMode = ScrollMode.Enabled, VerticalAlignment = VerticalAlignment.Center };
        panel.Children.Add(scroll); panel.Children.Add(empty);
        return panel;
    }

    private void Attach()
    {
        if (Studio is null || !IsLoaded || ReferenceEquals(_attached, Studio)) return;
        Detach(); _attached = Studio; _attached.PropertyChanged += OnStudioProperty; _signature = ""; Refresh();
    }
    private void Detach()
    {
        if (_attached is not null) _attached.PropertyChanged -= OnStudioProperty;
        _attached = null;
    }
    private void OnStudioProperty(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.GraphicsStrip) or nameof(StudioViewModel.GraphicsPlayoutStatus) or nameof(StudioViewModel.GraphicsPlayoutBusy)) Refresh();
    }

    private async Task Invoke(string actionId, params object?[] args)
    {
        if (_attached is not { } studio) return;
        var result = await GraphicsPlayoutActions.InvokeAsync(studio, actionId, args);
        if (!result.Ok) { _status.Text = result.Error ?? "Refused."; ToolTipService.SetToolTip(_status, _status.Text); }
        Refresh();
    }

    public void Refresh()
    {
        if (_attached is not { } studio) return;
        var strip = studio.GraphicsStrip; var busy = studio.GraphicsPlayoutBusy;
        _playOutAll.IsEnabled = !busy && strip.OnProgram.Count > 0; _clearPreview.IsEnabled = !busy && strip.OnPreview.Count > 0;
        if (studio.GraphicsPlayoutStatus.Length > 0) { _status.Text = studio.GraphicsPlayoutStatus; ToolTipService.SetToolTip(_status, _status.Text); }
        var signature = $"{busy}|{strip.AutomationArmed}:{strip.AutomationMode}|" + string.Join('|', strip.OnProgram.Concat(strip.OnPreview).Select(i => $"{i.InstanceId}:{i.Label}:{i.Owner}:{i.Visibility}:{i.CuePending}")) + "|" + strip.OnProgram.Count + ":" + strip.OnPreview.Count;
        if (signature == _signature) return;
        _signature = signature;
        _program.Children.Clear(); _preview.Children.Clear(); _auto.Children.Clear();
        foreach (var item in strip.OnProgram) _program.Children.Add(Chip(item, StudioBrushes.Air, "Hide", () => Invoke(GraphicsPlayoutActions.PlayOut, item.InstanceId), busy));
        foreach (var item in strip.OnPreview) _preview.Children.Add(Chip(item, StudioBrushes.Program, "Clear cue", () => Invoke(GraphicsPlayoutActions.PreviewRemove, item.InstanceId), busy));
        _programEmpty.Visibility = strip.OnProgram.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        _previewEmpty.Visibility = strip.OnPreview.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (strip.AutomationArmed) {
            var chip = ChipShell(StudioBrushes.Program);
            var body = (StackPanel)chip.Child;
            body.Children.Add(new TextBlock { Text = "Armed · " + strip.AutomationMode, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Program, VerticalAlignment = VerticalAlignment.Center });
            var disarm = StudioBrushes.ChipButton("Disarm"); disarm.IsEnabled = !busy;
            disarm.Click += async (_, _) => { if (_attached is { } s) { var ok = await s.GraphicsWorkspace.AutomationControlAsync("disarm"); if (!ok) _status.Text = s.GraphicsWorkspace.Status; } };
            body.Children.Add(disarm);
            ToolTipService.SetToolTip(chip, strip.Auto.Count == 0 ? "A native automation rule is armed." : "Armed rule targets: " + string.Join(", ", strip.Auto.Select(a => a.Label)));
            _auto.Children.Add(chip);
        }
    }

    private static Border ChipShell(Brush accent) => new() {
        Background = StudioBrushes.SurfaceRaised, BorderBrush = accent, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(14),
        Padding = new Thickness(10, 3, 4, 3), VerticalAlignment = VerticalAlignment.Center,
        Child = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 } };

    private static Border Chip(GraphicsStripItem item, Brush accent, string action, Func<Task> invoke, bool busy)
    {
        var chip = ChipShell(accent); var body = (StackPanel)chip.Child;
        body.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = accent, VerticalAlignment = VerticalAlignment.Center });
        body.Children.Add(new TextBlock { Text = item.Label, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center, MaxWidth = 180, TextTrimming = TextTrimming.CharacterEllipsis });
        if (item.Owner != "playlist") body.Children.Add(StudioBrushes.Badge(item.Owner));
        if (item.CuePending) body.Children.Add(StudioBrushes.Badge("preparing"));
        var button = StudioBrushes.ChipButton(action); button.IsEnabled = !busy; button.Click += async (_, _) => await invoke();
        body.Children.Add(button);
        ToolTipService.SetToolTip(chip, $"{item.Label} · {item.InstanceId} · r{item.Revision} · {item.Visibility}" + (item.AutoOutArmed ? " · hides automatically" : ""));
        return chip;
    }

    private static TextBlock Muted(string text) => new() { Text = text, FontSize = 11, Foreground = StudioBrushes.Muted, VerticalAlignment = VerticalAlignment.Center };
}

/// <summary>Studio palette for code-built graphics controls: the app's theme resources
/// (StudioTheme.xaml / App.xaml), never literal hex where a token exists.</summary>
internal static class StudioBrushes
{
    public static Brush Panel => Lookup("StudioPanelBrush", 0x10, 0x13, 0x15);
    public static Brush SurfaceRaised => Lookup("StudioSurfaceRaisedBrush", 0x1B, 0x1F, 0x22);
    public static Brush Field => Lookup("StudioFieldBrush", 0x0E, 0x11, 0x12);
    public static Brush Border => Lookup("StudioBorderBrush", 0x30, 0x3B, 0x46);
    public static Brush Accent => Lookup("StudioAccentBrush", 0x22, 0xC8, 0x6E);
    public static Brush Program => Lookup("StudioProgramBrush", 0xE8, 0xA4, 0x1F);
    public static Brush Air => Lookup("StudioAirBrush", 0xE5, 0x43, 0x3F);
    public static Brush Text => Lookup("StudioTextBrush", 0xE9, 0xED, 0xEF);
    public static Brush Muted => Lookup("StudioMutedBrush", 0x8B, 0x94, 0x9B);
    public static Brush Dim => Lookup("StudioDimBrush", 0x5C, 0x65, 0x6B);
    public static FontFamily Mono => Application.Current?.Resources.TryGetValue("StudioMonoFont", out var font) == true && font is FontFamily family ? family : new FontFamily("Consolas");

    private static Brush Lookup(string key, byte r, byte g, byte b) =>
        Application.Current?.Resources.TryGetValue(key, out var value) == true && value is Brush brush ? brush : new SolidColorBrush(Windows.UI.Color.FromArgb(255, r, g, b));

    public static Button GhostButton(string label) => new() { Content = label, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Padding = new Thickness(12, 6, 12, 6), MinHeight = 32,
        CornerRadius = new CornerRadius(8), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Foreground = Text, BorderBrush = Border, BorderThickness = new Thickness(1) };
    public static Button AccentButton(string label) { var b = GhostButton(label); b.Background = Accent; b.Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x06, 0x17, 0x0D)); b.BorderThickness = new Thickness(0); return b; }
    public static Button ChipButton(string label) { var b = GhostButton(label); b.FontSize = 11; b.Padding = new Thickness(8, 2, 8, 2); b.MinHeight = 22; b.CornerRadius = new CornerRadius(10); return b; }
    public static Border Badge(string text) => new() { Background = Field, BorderBrush = Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(4), Padding = new Thickness(5, 1, 5, 1), VerticalAlignment = VerticalAlignment.Center,
        Child = new TextBlock { Text = text, FontSize = 9, FontFamily = Mono, CharacterSpacing = 120, Foreground = Muted } };
    /// <summary>Hotkey badge: Consolas on purpose (the approved canvas), distinct from telemetry mono labels.</summary>
    public static Border HotkeyBadge(string hotkey) => new() { Width = 36, Height = 24, Background = Field, BorderBrush = Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(5), VerticalAlignment = VerticalAlignment.Center,
        Child = new TextBlock { Text = hotkey.Length == 0 ? "—" : hotkey, FontSize = 11, FontFamily = new FontFamily("Consolas"), FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = hotkey.Length == 0 ? Dim : Text, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } };
}
