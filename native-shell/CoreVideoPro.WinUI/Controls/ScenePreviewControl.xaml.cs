using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Text;
using FontWeight = Windows.UI.Text.FontWeight;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class ScenePreviewControl : UserControl
{
    public static readonly DependencyProperty SceneProperty =
        DependencyProperty.Register(
            nameof(Scene),
            typeof(Scene),
            typeof(ScenePreviewControl),
            new PropertyMetadata(null, OnLayoutInputsChanged));

    public static readonly DependencyProperty TilesProperty =
        DependencyProperty.Register(
            nameof(Tiles),
            typeof(IReadOnlyList<ParticipantSurfaceTile>),
            typeof(ScenePreviewControl),
            new PropertyMetadata(null, OnLayoutInputsChanged));

    public static readonly DependencyProperty IsPreviewProperty =
        DependencyProperty.Register(
            nameof(IsPreview),
            typeof(bool),
            typeof(ScenePreviewControl),
            new PropertyMetadata(false, OnIsPreviewChanged));

    public ScenePreviewControl()
    {
        InitializeComponent();
    }

    public Scene? Scene
    {
        get => (Scene?)GetValue(SceneProperty);
        set => SetValue(SceneProperty, value);
    }

    public IReadOnlyList<ParticipantSurfaceTile>? Tiles
    {
        get => (IReadOnlyList<ParticipantSurfaceTile>?)GetValue(TilesProperty);
        set => SetValue(TilesProperty, value);
    }

    public bool IsPreview
    {
        get => (bool)GetValue(IsPreviewProperty);
        set => SetValue(IsPreviewProperty, value);
    }

    private static void OnLayoutInputsChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args) =>
        ((ScenePreviewControl)sender).ApplyLayout();

    private static void OnIsPreviewChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ScenePreviewControl control)
        {
            control.Opacity = control.IsPreview ? 0.94 : 1;
        }
    }

    private void ApplyLayout()
    {
        LayoutRoot.Children.Clear();
        LayoutRoot.RowDefinitions.Clear();
        LayoutRoot.ColumnDefinitions.Clear();

        var tiles = Tiles ?? Array.Empty<ParticipantSurfaceTile>();
        var participants = tiles.Select(tile => tile.Participant).ToList();
        var layout = Scene?.Layout ?? "speaker-slides";

        switch (layout)
        {
            case "host-focus":
                BuildHostFocusLayout(participants, isOutro: false);
                break;
            case "outro":
                BuildHostFocusLayout(participants, isOutro: true);
                break;
            case "two-up":
                BuildTwoUpLayout(participants);
                break;
            case "smart-grid":
                BuildSmartGridLayout(participants);
                break;
            default:
                BuildSpeakerSlidesLayout(participants);
                break;
        }
    }

    private void BuildHostFocusLayout(IReadOnlyList<Participant> participants, bool isOutro)
    {
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.7, GridUnitType.Star) });
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        LayoutRoot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        LayoutRoot.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        LayoutRoot.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var host = participants.FirstOrDefault(p => p.Role == ParticipantRole.Host)
            ?? participants.FirstOrDefault();

        var heroTile = CreateTile(host, "hero");
        Grid.SetRow(heroTile, 0);
        Grid.SetColumn(heroTile, 0);
        Grid.SetRowSpan(heroTile, 3);
        LayoutRoot.Children.Add(heroTile);

        var kicker = CreateLabel(isOutro ? "Closing scene" : "Intro scene", 14, Microsoft.UI.Text.FontWeights.Bold, "#8FDCC8");
        Grid.SetColumn(kicker, 1);
        Grid.SetRow(kicker, 0);
        LayoutRoot.Children.Add(kicker);

        var title = CreateLabel(Scene?.Name ?? string.Empty, 36, Microsoft.UI.Text.FontWeights.Bold, "#F7FBF8");
        Grid.SetColumn(title, 1);
        Grid.SetRow(title, 1);
        LayoutRoot.Children.Add(title);

        var automation = CreateLabel(Scene?.Automation ?? string.Empty, 14, Microsoft.UI.Text.FontWeights.Normal, "#AEBEB5");
        Grid.SetColumn(automation, 1);
        Grid.SetRow(automation, 2);
        LayoutRoot.Children.Add(automation);
    }

    private void BuildTwoUpLayout(IReadOnlyList<Participant> participants)
    {
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        LayoutRoot.ColumnSpacing = 14;

        for (var index = 0; index < 2; index++)
        {
            var tile = CreateTile(participants.ElementAtOrDefault(index), "large");
            Grid.SetColumn(tile, index);
            LayoutRoot.Children.Add(tile);
        }
    }

    private void BuildSmartGridLayout(IReadOnlyList<Participant> participants)
    {
        const int columns = 3;
        var rows = (int)Math.Ceiling(participants.Count / (double)columns);
        if (rows < 1)
        {
            rows = 1;
        }

        for (var column = 0; column < columns; column++)
        {
            LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }

        for (var row = 0; row < rows; row++)
        {
            LayoutRoot.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        }

        LayoutRoot.ColumnSpacing = 14;
        LayoutRoot.RowSpacing = 14;

        for (var index = 0; index < participants.Count; index++)
        {
            var tile = CreateTile(participants[index], "grid");
            Grid.SetRow(tile, index / columns);
            Grid.SetColumn(tile, index % columns);
            LayoutRoot.Children.Add(tile);
        }
    }

    private void BuildSpeakerSlidesLayout(IReadOnlyList<Participant> participants)
    {
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.15, GridUnitType.Star) });
        LayoutRoot.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(0.85, GridUnitType.Star) });
        LayoutRoot.ColumnSpacing = 14;

        var speakerStack = new Grid
        {
            ColumnSpacing = 10,
            RowSpacing = 10
        };
        speakerStack.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        speakerStack.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        speakerStack.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        speakerStack.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });

        var tile0 = CreateTile(participants.ElementAtOrDefault(0), "stack");
        Grid.SetRow(tile0, 0);
        Grid.SetColumn(tile0, 0);
        speakerStack.Children.Add(tile0);

        var tile1 = CreateTile(participants.ElementAtOrDefault(1), "stack");
        Grid.SetRow(tile1, 0);
        Grid.SetColumn(tile1, 1);
        speakerStack.Children.Add(tile1);

        var tile2 = CreateTile(participants.ElementAtOrDefault(2), "stack");
        Grid.SetRow(tile2, 1);
        Grid.SetColumn(tile2, 0);
        speakerStack.Children.Add(tile2);

        var slidePanel = BuildSlideSharePanel();
        Grid.SetColumn(speakerStack, 0);
        Grid.SetColumn(slidePanel, 1);
        LayoutRoot.Children.Add(speakerStack);
        LayoutRoot.Children.Add(slidePanel);
    }

    private Border BuildSlideSharePanel()
    {
        var chart = new Grid { Margin = new Thickness(0, 20, 0, 0), MaxWidth = 420 };
        chart.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        chart.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        chart.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        chart.ColumnSpacing = 10;

        var bar0 = new Border { Height = 96, CornerRadius = new CornerRadius(8), Background = BrushFromHex("#44C1A1") };
        var bar1 = new Border
        {
            Height = 132,
            VerticalAlignment = VerticalAlignment.Bottom,
            CornerRadius = new CornerRadius(8),
            Background = BrushFromHex("#F0A85C")
        };
        var bar2 = new Border
        {
            Height = 68,
            VerticalAlignment = VerticalAlignment.Bottom,
            CornerRadius = new CornerRadius(8),
            Background = BrushFromHex("#263931")
        };
        Grid.SetColumn(bar1, 1);
        Grid.SetColumn(bar2, 2);
        chart.Children.Add(bar0);
        chart.Children.Add(bar1);
        chart.Children.Add(bar2);

        var panel = new StackPanel { Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        panel.Children.Add(CreateLabel("Q2 PRODUCT UPDATE", 11, Microsoft.UI.Text.FontWeights.Bold, "#8FDCC8"));
        panel.Children.Add(CreateLabel("Building what matters next", 26, Microsoft.UI.Text.FontWeights.Bold, "#EDF4EF"));
        panel.Children.Add(chart);
        panel.Children.Add(CreateLabel("• Customer-obsessed roadmap", 13, Microsoft.UI.Text.FontWeights.Normal, "#AEBEB5"));
        panel.Children.Add(CreateLabel("• Platform scalability & reliability", 13, Microsoft.UI.Text.FontWeights.Normal, "#AEBEB5"));
        panel.Children.Add(CreateLabel("• Ecosystem partnerships", 13, Microsoft.UI.Text.FontWeights.Normal, "#AEBEB5"));
        panel.Children.Add(CreateLabel("COREVIDEO", 11, Microsoft.UI.Text.FontWeights.Bold, "#44C1A1"));

        return new Border
        {
            CornerRadius = new CornerRadius(8),
            BorderBrush = BrushFromHex("#2E44C1A1"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(28),
            Background = new LinearGradientBrush
            {
                StartPoint = new Windows.Foundation.Point(0, 0),
                EndPoint = new Windows.Foundation.Point(1, 1),
                GradientStops =
                {
                    new GradientStop { Color = Windows.UI.Color.FromArgb(245, 12, 20, 24), Offset = 0 },
                    new GradientStop { Color = Windows.UI.Color.FromArgb(245, 8, 14, 18), Offset = 1 }
                }
            },
            Child = panel
        };
    }

    private ParticipantTileControl CreateTile(Participant? participant, string variant)
    {
        var tile = Tiles?.FirstOrDefault(entry => entry.Participant.Id == participant?.Id);
        return new ParticipantTileControl
        {
            Participant = participant,
            TileVariant = variant,
            SurfaceState = tile?.Surface,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch
        };
    }

    private static TextBlock CreateLabel(string text, double fontSize, FontWeight weight, string color) =>
        new()
        {
            Text = text,
            FontSize = fontSize,
            FontWeight = weight,
            Foreground = BrushFromHex(color),
            TextWrapping = TextWrapping.Wrap
        };

    private static SolidColorBrush BrushFromHex(string hex)
    {
        hex = hex.TrimStart('#');
        if (hex.Length == 6)
        {
            hex = "FF" + hex;
        }

        var value = Convert.ToUInt32(hex, 16);
        return new SolidColorBrush(Windows.UI.Color.FromArgb(
            (byte)((value >> 24) & 0xFF),
            (byte)((value >> 16) & 0xFF),
            (byte)((value >> 8) & 0xFF),
            (byte)(value & 0xFF)));
    }
}