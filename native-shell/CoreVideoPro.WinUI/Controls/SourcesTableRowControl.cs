using System.ComponentModel;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>A retained input row. Resizing/filtering never replaces its controls or source surface.</summary>
internal sealed class SourcesTableRowControl : StackPanel, IDisposable
{
    internal ShowInputSlotViewModel Input { get; }
    private readonly Grid _grid = new() { ColumnSpacing = 6, Padding = new Thickness(8, 6, 8, 6) };
    private readonly FrameworkElement[] _cells;
    private readonly Border _thumbnail;
    private readonly Action _identityChanged;
    internal static readonly double[] WideWidths = [30, -1, 78, 100, 45, 90, 86, 40, 66, 108];

    internal SourcesTableRowControl(ShowInputSlotViewModel input, Action<ShowInputSlotViewModel> inspect,
        Action<ShowInputSlotViewModel> grade, Action identityChanged)
    {
        Input = input; _identityChanged = identityChanged; Spacing = 0;
        var number = Text(input.SlotNumber.ToString("00"));
        var identity = new Grid { ColumnSpacing = 8 };
        identity.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        identity.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        _thumbnail = new Border { Width = 48, Height = 30, Background = Brush(18, 27, 34), CornerRadius = new(3) };
        var surface = new VideoSurfaceHost { SourceFit = "fit", IsHitTestVisible = false };
        Bind(surface, VideoSurfaceHost.SurfaceStateProperty, input, "Inspector.PreviewSurface");
        surface.SetBinding(UIElement.VisibilityProperty, new Binding {Source=input,Path=new PropertyPath("Inspector.HasPreviewTile"),Converter=new Converters.BoolToVisibilityConverter()});
        _thumbnail.BorderThickness = new(2); Bind(_thumbnail, Border.BorderBrushProperty, input, "Inspector.PreviewBorderBrush");
        _thumbnail.Child = surface; identity.Children.Add(_thumbnail);
        var names = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        var name = Text(""); name.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        Bind(name, TextBlock.TextProperty, input, "OverviewName");
        var description = Text(""); description.FontSize = 10; description.Foreground = Brush(163, 187, 200);
        Bind(description, TextBlock.TextProperty, input, "KindLabel");
        names.Children.Add(name); names.Children.Add(description); Grid.SetColumn(names, 1); identity.Children.Add(names);
        ToolTipService.SetToolTip(identity, input.SelectedUnifiedSourceLabel);
        var type = Text(""); Bind(type, TextBlock.TextProperty, input, "KindLabel");
        var resolution = Text(""); resolution.FontFamily = new("Consolas");
        Bind(resolution, TextBlock.TextProperty, input, "Inspector.ResolutionLabel");
        Bind(resolution, TextBlock.ForegroundProperty, input, "Inspector.FormatBrush");
        var fps = Text(""); fps.FontFamily = new("Consolas");
        Bind(fps, TextBlock.TextProperty, input, "Inspector.FrameRateLabel");
        var status = Text(""); Bind(status, TextBlock.TextProperty, input, "Inspector.StatusChipLabel");
        var statusChip = new Border { Child = status, CornerRadius = new(3), Padding = new(5, 3, 5, 3), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Center };
        Bind(statusChip, Border.BackgroundProperty, input, "Inspector.StatusBackgroundBrush");
        var audio = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        var meter = new Border { Height = 7, Width = 80, Background = Brush(43, 59, 69), HorizontalAlignment = HorizontalAlignment.Left };
        var level = new Rectangle { Height = 7, Fill = Brush(114, 217, 170), HorizontalAlignment = HorizontalAlignment.Left };
        Bind(level, FrameworkElement.WidthProperty, input, "Inspector.MeterWidth"); meter.Child = level;
        var audioLabel = Text(""); audioLabel.FontSize = 10;
        Bind(audioLabel, TextBlock.TextProperty, input, "AudioSummary"); audio.Children.Add(meter); audio.Children.Add(audioLabel);
        var mv = new CheckBox { MinWidth = 0, Padding = new(0), VerticalAlignment = VerticalAlignment.Center };
        AutomationProperties.SetName(mv, $"Input {input.SlotNumber} in Multiview");
        Bind(mv, CheckBox.IsCheckedProperty, input, "InShow", BindingMode.TwoWay);
        Bind(mv, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, input, "IsAssigned");
        ToolTipService.SetToolTip(mv, "Include this input in Studio Multiview. Does not Take a scene.");
        var iso = new StackPanel { Spacing = 0, VerticalAlignment = VerticalAlignment.Center };
        var arm = new CheckBox { MinWidth = 0, Padding = new(0) };
        AutomationProperties.SetName(arm, $"Arm ISO for input {input.SlotNumber}");
        Bind(arm, CheckBox.IsCheckedProperty, input, "IsoEnabled", BindingMode.TwoWay);
        Bind(arm, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, input, "ShowIsoToggle");
        ToolTipService.SetToolTip(arm, "Select a source for Program + ISOs. Arming is separate from recording state.");
        var writer = Text(""); writer.FontSize = 9; Bind(writer, TextBlock.TextProperty, input, "Inspector.RecChipLabel");
        var writerChip = new Border {Child = writer, CornerRadius = new(3), Padding = new(3), HorizontalAlignment = HorizontalAlignment.Left};
        Bind(writerChip, Border.BackgroundProperty, input, "Inspector.RecBackgroundBrush");
        iso.Children.Add(arm); iso.Children.Add(writerChip);
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        var edit = Button("Inspect", () => inspect(input)); AutomationProperties.SetName(edit, $"Inspect input {input.SlotNumber}");
        var gradeButton = Button("Grade", () => grade(input));
        Bind(gradeButton, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, input, "IsAssigned");
        Bind(gradeButton, FrameworkElement.TagProperty, input, "SourceColorGradeId");
        AutomationProperties.SetName(gradeButton, $"Grade input {input.SlotNumber}"); actions.Children.Add(edit); actions.Children.Add(gradeButton);
        _cells = [number, identity, type, resolution, fps, statusChip, audio, mv, iso, actions];
        for (var i = 0; i < _cells.Length; i++) { _grid.ColumnDefinitions.Add(new()); Grid.SetColumn(_cells[i], i); _grid.Children.Add(_cells[i]); }
        Children.Add(_grid);
        var details = new Border { Visibility = Visibility.Collapsed, Padding = new(12), Background = Brush(26, 45, 55) };
        var more = Button("⋯", () => details.Visibility = details.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible);
        AutomationProperties.SetName(more, $"More details for input {input.SlotNumber}"); actions.Children.Add(more);
        var detailContent = new StackPanel { Spacing = 4 };
        foreach (var (label, path) in new[] { ("Source / assignment", "SelectedUnifiedSourceLabel"), ("Source state", "Inspector.StatusChipLabel"),
            ("Last observed format", "Inspector.FormatLabel"), ("Request / freshness", "Inspector.FormatDetailLabel"),
            ("Paired audio", "AudioSummary"), ("ISO writer", "Inspector.RecChipLabel"), ("Recovery", "AssignmentHint") })
        {
            var line = new StackPanel { Spacing = 1 }; line.Children.Add(Text(label, 10));
            var value = Text(""); value.TextWrapping = TextWrapping.Wrap; Bind(value, TextBlock.TextProperty, input, path); line.Children.Add(value); detailContent.Children.Add(line);
        }
        details.Child = detailContent; Children.Add(details);
        input.PropertyChanged += OnInputChanged;
    }

    private void OnInputChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ShowInputSlotViewModel.DisplayName) or nameof(ShowInputSlotViewModel.KindLabel) or
            nameof(ShowInputSlotViewModel.SelectedUnifiedSourceLabel) or nameof(ShowInputSlotViewModel.InShow) or nameof(ShowInputSlotViewModel.IsoEnabled))
        { ToolTipService.SetToolTip(_cells[1], Input.SelectedUnifiedSourceLabel); _identityChanged(); }
    }
    internal void SetSelected(bool selected) => Background = selected ? Brush(35, 65, 57) : Brush(20, 31, 39);
    internal void Layout(double width, Services.SourcesWorkspacePreferences preferences)
    {
        var widths = ColumnWidths(width, preferences);
        for (var i = 0; i < widths.Length; i++)
        {
            _grid.ColumnDefinitions[i].Width = widths[i] < 0 ? new(1, GridUnitType.Star) : new(widths[i]);
            _cells[i].Visibility = widths[i] == 0 ? Visibility.Collapsed : Visibility.Visible;
        }
        _thumbnail.Visibility = width >= 750 ? Visibility.Visible : Visibility.Collapsed;
    }
    internal static double[] ColumnWidths(double width, Services.SourcesWorkspacePreferences p) =>
        [26, -1, p.ShowType && width >= 1050 ? Math.Max(78, width * .08) : 0,
         Math.Max(width >= 750 ? 100 : 85, width * .13), Math.Max(42, width * .05),
         p.ShowSignal && width >= 750 ? Math.Max(86, width * .09) : 0,
         p.ShowAudio && width >= 1050 ? Math.Max(86, width * .12) : 0,
         Math.Max(36, width * .04), Math.Max(60, width * .06), 140];
    public void Dispose() => Input.PropertyChanged -= OnInputChanged;
    internal static SolidColorBrush Brush(byte r, byte g, byte b) => new(Windows.UI.Color.FromArgb(255, r, g, b));
    internal static TextBlock Text(string text, double size = 12) => new()
    { Text = text, FontSize = size, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center };
    internal static Button Button(string text, Action action)
    { var button = new Button { Content = text, FontSize = 11, Padding = new(7, 5, 7, 5), Background = Brush(32,49,59), Foreground = Brush(231,240,243), BorderBrush = Brush(71,96,110) }; button.Click += (_, _) => action(); return button; }
    internal static void Bind(FrameworkElement element, DependencyProperty property, object source, string path, BindingMode mode = BindingMode.OneWay) =>
        element.SetBinding(property, new Binding { Source = source, Path = new PropertyPath(path), Mode = mode });
}
