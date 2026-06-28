using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

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

    public static readonly DependencyProperty SourceScaleProperty =
        DependencyProperty.Register(
            nameof(SourceScale),
            typeof(double),
            typeof(ParticipantTileControl),
            new PropertyMetadata(1d, OnSourceFramingChanged));

    public static readonly DependencyProperty SourceOffsetXProperty =
        DependencyProperty.Register(
            nameof(SourceOffsetX),
            typeof(double),
            typeof(ParticipantTileControl),
            new PropertyMetadata(0d, OnSourceFramingChanged));

    public static readonly DependencyProperty SourceOffsetYProperty =
        DependencyProperty.Register(
            nameof(SourceOffsetY),
            typeof(double),
            typeof(ParticipantTileControl),
            new PropertyMetadata(0d, OnSourceFramingChanged));

    // Opt-in GPU rendering for program/preview scene layers. When true and the surface
    // carries a valid GPU shared-texture handle, the embedded VideoSurfaceHost presents
    // it (1080p, self-refreshing via keyed mutex) instead of the CPU base64 image. Left
    // off for the many other ParticipantTileControl uses (source picker, etc.) so they
    // don't each spin up a D3D swap chain.
    public static readonly DependencyProperty UseGpuSurfaceProperty =
        DependencyProperty.Register(
            nameof(UseGpuSurface),
            typeof(bool),
            typeof(ParticipantTileControl),
            new PropertyMetadata(false, OnSurfaceStateChanged));

    public ParticipantTileControl()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        SizeChanged += OnSizeChanged;
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

    public bool UseGpuSurface
    {
        get => (bool)GetValue(UseGpuSurfaceProperty);
        set => SetValue(UseGpuSurfaceProperty, value);
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

    public double SourceScale
    {
        get => (double)GetValue(SourceScaleProperty);
        set => SetValue(SourceScaleProperty, value);
    }

    public double SourceOffsetX
    {
        get => (double)GetValue(SourceOffsetXProperty);
        set => SetValue(SourceOffsetXProperty, value);
    }

    public double SourceOffsetY
    {
        get => (double)GetValue(SourceOffsetYProperty);
        set => SetValue(SourceOffsetYProperty, value);
    }

    public string ParticipantName => Participant?.Name ?? string.Empty;

    public string Initials => Participant?.Initials ?? string.Empty;

    public bool IsActiveSpeaker => Participant?.IsActiveSpeaker == true;

    public bool IsScreenSharing => Participant?.IsScreenSharing == true;

    public string SurfaceKey => SurfaceState?.SurfaceKey ?? Participant?.Id ?? "tile";

    public string? MediaAssetPath => SurfaceState?.MediaAssetPath;

    public string? MediaAssetKind => SurfaceState?.MediaAssetKind;

    public bool MediaAssetPlaying => SurfaceState?.MediaAssetPlaying == true;

    public string? MediaPlaybackKey => SurfaceState?.MediaPlaybackKey ?? SurfaceState?.SurfaceKey;

    public bool HasMediaAssetPreview => !string.IsNullOrWhiteSpace(SurfaceState?.MediaAssetPath);

    private bool _sourceFramingRefreshScheduled;

    public void RefreshSourceFraming()
    {
        ApplySourceFraming();
        ScheduleSourceFramingRefresh();
    }

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
            tile.Bindings.Update();
            tile.UpdatePreviewBitmap();
            tile.ApplySourceFraming();
            tile.ScheduleSourceFramingRefresh();
        }
    }

    private static void OnVisualChromeChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.ApplySourceFit();
            tile.ApplyActiveSpeakerStyle();
            tile.ScheduleSourceFramingRefresh();
        }
    }

    private static void OnSourceFramingChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ParticipantTileControl tile)
        {
            tile.ApplySourceFraming();
            tile.ScheduleSourceFramingRefresh();
        }
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        RefreshSourceFraming();
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        RefreshSourceFraming();
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
        // GPU path (program/preview scene layers): if this surface has a live shared
        // texture, present it through the embedded VideoSurfaceHost. The handle is stable
        // and the keyed-mutex texture refreshes itself every vsync, so program/preview
        // stay live without any per-frame tile rebuild (which would churn the GPU hosts).
        if (UseGpuSurface && SurfaceState?.PendingSharedHandle is { IsValid: true })
        {
            GpuVideoHost.SurfaceState = SurfaceState;
            GpuVideoHost.Visibility = Visibility.Visible;
            BgraPreviewHelper.SetPreview(PreviewImage, null, 0, 0);
            PreviewImage.Visibility = Visibility.Collapsed;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            InitialsBadge.Visibility = Visibility.Collapsed;
            InitialsText.Visibility = Visibility.Collapsed;
            return;
        }

        if (GpuVideoHost.Visibility == Visibility.Visible)
        {
            // Fell out of the GPU path (handle dropped / not a GPU layer): release it.
            GpuVideoHost.Visibility = Visibility.Collapsed;
            GpuVideoHost.SurfaceState = null;
        }

        if (SurfaceState is null)
        {
            BgraPreviewHelper.SetPreview(PreviewImage, null, 0, 0);
            PlaceholderPanel.Visibility = Visibility.Visible;
            InitialsBadge.Visibility = Visibility.Visible;
            InitialsText.Visibility = Visibility.Visible;
            ScheduleSourceFramingRefresh();
            return;
        }

        if (HasMediaAssetPreview)
        {
            BgraPreviewHelper.SetPreview(PreviewImage, null, 0, 0);
            PreviewImage.Visibility = Visibility.Collapsed;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            InitialsBadge.Visibility = Visibility.Collapsed;
            InitialsText.Visibility = Visibility.Collapsed;
            ScheduleSourceFramingRefresh();
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
        ScheduleSourceFramingRefresh();
    }

    private void ApplySourceFit()
    {
        ApplySourceFraming();
    }

    private void ApplySourceFraming()
    {
        var viewportWidth = TileViewport.ActualWidth > 0 ? TileViewport.ActualWidth : ActualWidth;
        var viewportHeight = TileViewport.ActualHeight > 0 ? TileViewport.ActualHeight : ActualHeight;
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        TileViewport.Clip = new RectangleGeometry
        {
            Rect = new Rect(0, 0, viewportWidth, viewportHeight)
        };
        SourceLayerCanvas.Width = viewportWidth;
        SourceLayerCanvas.Height = viewportHeight;

        var layout = SourceFramingLayoutService.Resolve(
            viewportWidth,
            viewportHeight,
            SurfaceState?.FramingSourceWidth ?? 0,
            SurfaceState?.FramingSourceHeight ?? 0,
            SourceFit,
            SourceScale,
            SourceOffsetX,
            SourceOffsetY);

        PreviewImage.Width = layout.Width;
        PreviewImage.Height = layout.Height;
        PreviewImage.RenderTransform = null;
        Canvas.SetLeft(PreviewImage, layout.TranslateX);
        Canvas.SetTop(PreviewImage, layout.TranslateY);

        MediaPreview.Width = layout.Width;
        MediaPreview.Height = layout.Height;
        MediaPreview.RenderTransform = null;
        Canvas.SetLeft(MediaPreview, layout.TranslateX);
        Canvas.SetTop(MediaPreview, layout.TranslateY);
    }

    private void ScheduleSourceFramingRefresh()
    {
        if (_sourceFramingRefreshScheduled)
        {
            return;
        }

        _sourceFramingRefreshScheduled = true;
        _ = DispatcherQueue.TryEnqueue(() =>
        {
            _sourceFramingRefreshScheduled = false;
            ApplySourceFraming();
        });
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
