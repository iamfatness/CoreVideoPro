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

    private void ApplyActiveSpeakerStyle()
    {
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
}