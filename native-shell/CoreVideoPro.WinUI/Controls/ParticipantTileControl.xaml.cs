using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class ParticipantTileControl : UserControl
{
    public static readonly DependencyProperty ParticipantProperty =
        DependencyProperty.Register(
            nameof(Participant),
            typeof(Participant),
            typeof(ParticipantTileControl),
            new PropertyMetadata(null, OnParticipantChanged));

    public static readonly DependencyProperty TileVariantProperty =
        DependencyProperty.Register(
            nameof(TileVariant),
            typeof(string),
            typeof(ParticipantTileControl),
            new PropertyMetadata("stack", OnTileVariantChanged));

    public static readonly DependencyProperty SurfaceStateProperty =
        DependencyProperty.Register(
            nameof(SurfaceState),
            typeof(VideoSurfaceState),
            typeof(ParticipantTileControl),
            new PropertyMetadata(null, OnSurfaceStateChanged));

    public static readonly DependencyProperty SourceFitProperty =
        DependencyProperty.Register(
            nameof(SourceFit),
            typeof(string),
            typeof(ParticipantTileControl),
            new PropertyMetadata("fill", OnVisualChromeChanged));

    public static readonly DependencyProperty SourceBorderStyleProperty =
        DependencyProperty.Register(
            nameof(SourceBorderStyle),
            typeof(string),
            typeof(ParticipantTileControl),
            new PropertyMetadata("default", OnVisualChromeChanged));

    public static readonly DependencyProperty SourceBorderColorProperty =
        DependencyProperty.Register(
            nameof(SourceBorderColor),
            typeof(string),
            typeof(ParticipantTileControl),
            new PropertyMetadata("#44C1A1", OnVisualChromeChanged));

    public static readonly DependencyProperty SourceBorderThicknessProperty =
        DependencyProperty.Register(
            nameof(SourceBorderThickness),
            typeof(double),
            typeof(ParticipantTileControl),
            new PropertyMetadata(1d, OnVisualChromeChanged));

    public ParticipantTileControl()
    {
        InitializeComponent();
    }

    public Participant? Participant
    {
        get => (Participant?)GetValue(ParticipantProperty);
        set => SetValue(ParticipantProperty, value);
    }

    public string TileVariant
    {
        get => (string)GetValue(TileVariantProperty);
        set => SetValue(TileVariantProperty, value);
    }

    public VideoSurfaceState? SurfaceState
    {
        get => (VideoSurfaceState?)GetValue(SurfaceStateProperty);
        set => SetValue(SurfaceStateProperty, value);
    }

    public string SourceFit
    {
        get => (string)GetValue(SourceFitProperty);
        set => SetValue(SourceFitProperty, value);
    }

    public string SourceBorderStyle
    {
        get => (string)GetValue(SourceBorderStyleProperty);
        set => SetValue(SourceBorderStyleProperty, value);
    }

    public string SourceBorderColor
    {
        get => (string)GetValue(SourceBorderColorProperty);
        set => SetValue(SourceBorderColorProperty, value);
    }

    public double SourceBorderThickness
    {
        get => (double)GetValue(SourceBorderThicknessProperty);
        set => SetValue(SourceBorderThicknessProperty, value);
    }

    public string ParticipantName => Participant?.Name ?? string.Empty;

    public string Initials => Participant?.Initials ?? string.Empty;

    public bool IsActiveSpeaker => Participant?.IsActiveSpeaker == true;

    public bool IsScreenSharing => Participant?.IsScreenSharing == true;

    private static void OnParticipantChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.Bindings.Update();
            tile.ApplyActiveSpeakerStyle();
        }
    }

    private static void OnTileVariantChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.ApplyVariantStyle();
        }
    }

    private static void OnSurfaceStateChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.UpdatePreviewBitmap();
        }
    }

    private static void OnVisualChromeChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.ApplySourceFit();
            tile.ApplyActiveSpeakerStyle();
        }
    }

    private void ApplyActiveSpeakerStyle()
    {
        var borderStyle = SourceBorderStyle;
        if (borderStyle is "none")
        {
            TileBorder.BorderThickness = new Thickness(0);
            return;
        }

        if (borderStyle is "solid" or "accent" or "program" or "warning")
        {
            TileBorder.BorderBrush = BrushForBorderStyle(borderStyle, SourceBorderColor);
            TileBorder.BorderThickness = new Thickness(Math.Clamp(SourceBorderThickness, 0, 12));
            return;
        }

        if (IsActiveSpeaker)
        {
            TileBorder.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92));
            TileBorder.BorderThickness = new Thickness(2);
        }
        else
        {
            TileBorder.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(31, 237, 244, 239));
            TileBorder.BorderThickness = new Thickness(1);
        }
    }

    private void ApplyVariantStyle()
    {
        TileBorder.MinHeight = 0;
    }

    private void UpdatePreviewBitmap()
    {
        if (SurfaceState is null)
        {
            BgraPreviewHelper.SetPreview(PreviewImage, null, 0, 0);
            PlaceholderPanel.Visibility = Visibility.Visible;
            InitialsBadge.Visibility = Visibility.Visible;
            InitialsText.Visibility = Visibility.Visible;
            return;
        }

        BgraPreviewHelper.SetPreview(
            PreviewImage,
            SurfaceState.PreviewBgra,
            SurfaceState.PreviewWidth,
            SurfaceState.PreviewHeight);

        var hasPreview = SurfaceState.HasPreviewBitmap;
        PlaceholderPanel.Visibility = hasPreview ? Visibility.Collapsed : Visibility.Visible;
        InitialsBadge.Visibility = hasPreview ? Visibility.Collapsed : Visibility.Visible;
        InitialsText.Visibility = hasPreview ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ApplySourceFit()
    {
        PreviewImage.Stretch = SourceFit switch
        {
            "fit" => Stretch.Uniform,
            "stretch" => Stretch.Fill,
            _ => Stretch.UniformToFill
        };
    }

    private static SolidColorBrush BrushForBorderStyle(string style, string color) =>
        style switch
        {
            "program" => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92)),
            "warning" => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 224, 90, 90)),
            "accent" => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 68, 193, 161)),
            _ => BrushFromHex(color)
        };

    private static SolidColorBrush BrushFromHex(string hex)
    {
        hex = (hex ?? "#44C1A1").Trim().TrimStart('#');
        if (hex.Length == 6)
        {
            hex = "FF" + hex;
        }

        if (hex.Length != 8 || !uint.TryParse(hex, System.Globalization.NumberStyles.HexNumber, null, out var value))
        {
            value = 0xFF44C1A1;
        }

        return new SolidColorBrush(Windows.UI.Color.FromArgb(
            (byte)((value >> 24) & 0xFF),
            (byte)((value >> 16) & 0xFF),
            (byte)((value >> 8) & 0xFF),
            (byte)(value & 0xFF)));
    }
}
