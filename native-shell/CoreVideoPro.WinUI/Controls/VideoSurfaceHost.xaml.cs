using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Foundation;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class VideoSurfaceHost : UserControl, IVideoSurfacePresenter, IDirect3DVideoSurfaceHost
{
    public static readonly DependencyProperty SurfaceStateProperty =
        DependencyProperty.Register(
            nameof(SurfaceState),
            typeof(VideoSurfaceState),
            typeof(VideoSurfaceHost),
            new PropertyMetadata(null, OnSurfaceStateChanged));

    public static readonly DependencyProperty SourceFitProperty =
        DependencyProperty.Register(
            nameof(SourceFit),
            typeof(string),
            typeof(VideoSurfaceHost),
            new PropertyMetadata("fit", OnSourceFitChanged));

    public static readonly DependencyProperty SourceScaleProperty =
        DependencyProperty.Register(
            nameof(SourceScale),
            typeof(double),
            typeof(VideoSurfaceHost),
            new PropertyMetadata(1d, OnSourceFramingChanged));

    public static readonly DependencyProperty SourceOffsetXProperty =
        DependencyProperty.Register(
            nameof(SourceOffsetX),
            typeof(double),
            typeof(VideoSurfaceHost),
            new PropertyMetadata(0d, OnSourceFramingChanged));

    public static readonly DependencyProperty SourceOffsetYProperty =
        DependencyProperty.Register(
            nameof(SourceOffsetY),
            typeof(double),
            typeof(VideoSurfaceHost),
            new PropertyMetadata(0d, OnSourceFramingChanged));

    private readonly Direct3D11InteropService _direct3DInterop = new();
    private nint _direct3DDevicePointer;
    private bool _refreshingPathBindings;
    private bool _sourceFramingRefreshScheduled;
    private string? _lastPreviewSurfaceKey;

    public VideoSurfaceHost()
    {
        InitializeComponent();
        _direct3DInterop.PresentationPathChanged += OnPresentationPathChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
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

    public string SurfaceKey => SurfaceState?.SurfaceKey ?? "unknown";

    public string Title => SurfaceState?.Title ?? "Video";

    public string StatusLine => SurfaceState?.StatusLine ?? "Waiting for frames";

    public string DetailLine => SurfaceState?.DetailLine ?? "Start the engine to receive media-core frames.";

    public bool HasFrameStats => SurfaceState?.LastFrame is not null;

    public string ParticipantLine =>
        SurfaceState?.LastFrame?.ParticipantId is { Length: > 0 } participantId
            ? $"Participant {participantId}"
            : SurfaceState?.Kind switch
            {
                VideoSurfaceKind.Preview => "Preview output",
                VideoSurfaceKind.Multiview => "Multiview output",
                VideoSurfaceKind.Participant => "Participant output",
                _ => "Program output"
            };

    public string ResolutionLine =>
        SurfaceState?.LastFrame is { } frame
            ? $"{frame.ResolutionLabel} · frame {frame.FrameId}"
            : string.Empty;

    public string FpsLine =>
        SurfaceState?.LastFrame is { } frame
            ? $"{frame.FpsLabel} · {frame.Health}"
            : string.Empty;

    public string RendererLine =>
        SurfaceState?.LastFrame is { } frame
            ? $"Renderer {frame.Renderer}"
            : string.Empty;

    public bool ShowPathBadge =>
        IsProgramSurface &&
        (IsGpuPathActive || IsCpuFallbackActive || (SurfaceState?.AwaitingDirect3D == true && IsDirect3DReady));

    public string PathBadgeText =>
        IsGpuPathActive
            ? "GPU · full-res"
            : IsCpuFallbackActive
                ? "CPU · BGRA preview"
                : SurfaceState?.AwaitingDirect3D == true && IsDirect3DReady
                    ? "GPU · waiting for handle"
                    : string.Empty;

    public Brush PathBadgeForeground =>
        IsGpuPathActive
            ? (Brush)Application.Current.Resources["StudioAccentBrush"]
            : IsCpuFallbackActive
                ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92))
                : (Brush)Application.Current.Resources["StudioMutedBrush"];

    public bool HasPreviewBitmap => SurfaceState?.HasPreviewBitmap == true;

    public string? MediaAssetPath => SurfaceState?.MediaAssetPath;

    public string? MediaAssetKind => SurfaceState?.MediaAssetKind;

    public bool MediaAssetPlaying => SurfaceState?.MediaAssetPlaying == true;

    public bool HasMediaAssetPreview => !string.IsNullOrWhiteSpace(SurfaceState?.MediaAssetPath);

    public bool IsDirect3DReady => _direct3DInterop.IsReady || _direct3DDevicePointer != 0;

    public bool IsGpuPathActive => _direct3DInterop.IsGpuPresenting;

    public bool IsCpuFallbackActive =>
        _direct3DInterop.IsCpuFallback ||
        (SurfaceState?.PendingSharedHandle is { IsValid: true } && !IsGpuPathActive && HasPreviewBitmap);

    public void UpdateMetadata(VideoFrameMetadata metadata, string statusLine, string? detailLine = null)
    {
        var current = SurfaceState ?? VideoSurfaceState.Waiting(VideoSurfaceKind.Program, SurfaceKey, Title);
        SurfaceState = current.WithFrame(metadata, statusLine, detailLine);
    }

    public bool TryAcceptSharedTextureHandle(SharedTextureHandle handle)
    {
        if (!handle.IsValid)
        {
            return false;
        }

        var current = SurfaceState ?? VideoSurfaceState.Waiting(VideoSurfaceKind.Program, SurfaceKey, Title);
        SurfaceState = current.WithSharedHandle(handle);

        if (IsDirect3DReady)
        {
            return PresentSharedHandle(handle);
        }

        return false;
    }

    public void Clear()
    {
        var kind = SurfaceState?.Kind ?? VideoSurfaceKind.Program;
        SurfaceState = VideoSurfaceState.Waiting(kind, SurfaceKey, Title);
        _direct3DDevicePointer = 0;
        SwapChainHost.Visibility = Visibility.Collapsed;
        BgraPreviewHelper.ClearPreview(PreviewImage);
        _lastPreviewSurfaceKey = null;
        PreviewImage.Visibility = Visibility.Collapsed;
    }

    public void RefreshSourceFraming()
    {
        ApplySourceFraming();
        ScheduleSourceFramingRefresh();
    }

    public void SetDirect3DDevice(nint devicePointer)
    {
        _direct3DDevicePointer = devicePointer;
        if (devicePointer != 0 || _direct3DInterop.IsReady)
        {
            SwapChainHost.Visibility = Visibility.Visible;
        }
    }

    public bool PresentSharedHandle(SharedTextureHandle handle)
    {
        if (!handle.IsValid)
        {
            return false;
        }

        if (_direct3DInterop.TryPresentSharedTexture(handle))
        {
            SwapChainHost.Visibility = Visibility.Visible;
            PreviewImage.Visibility = Visibility.Collapsed;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            RefreshPathBindings();
            return true;
        }

        UpdatePreviewBitmap();
        RefreshPathBindings();
        return false;
    }

    private static void OnSurfaceStateChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is VideoSurfaceHost host)
        {
            host.TryPresentPendingSharedHandle();
            host.UpdatePreviewBitmap();
            host.ApplySourceFraming();
            host.ScheduleSourceFramingRefresh();
            host.RefreshPathBindings();
        }
    }

    private static void OnSourceFitChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is VideoSurfaceHost host)
        {
            host.ApplySourceFit();
            host.ScheduleSourceFramingRefresh();
        }
    }

    private static void OnSourceFramingChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is VideoSurfaceHost host)
        {
            host.ApplySourceFraming();
            host.ScheduleSourceFramingRefresh();
        }
    }

    private bool IsProgramSurface =>
        string.Equals(SurfaceKey, "program", StringComparison.Ordinal) ||
        SurfaceState?.Kind == VideoSurfaceKind.Program;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        RefreshSourceFraming();

        if (!IsProgramSurface)
        {
            return;
        }

        if (_direct3DInterop.TryAttachSwapChainPanel(SwapChainHost))
        {
            _direct3DDevicePointer = _direct3DInterop.DevicePointer;
            SetDirect3DDevice(_direct3DDevicePointer);
            TryPresentPendingSharedHandle();
            RefreshPathBindings();
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _direct3DInterop.Dispose();
        _direct3DDevicePointer = 0;
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        RefreshSourceFraming();
    }

    private void OnPresentationPathChanged() =>
        DispatcherQueue.TryEnqueue(RefreshPathBindings);

    private void TryPresentPendingSharedHandle()
    {
        if (!IsProgramSurface || SurfaceState?.PendingSharedHandle is not { IsValid: true } handle)
        {
            return;
        }

        PresentSharedHandle(handle);
    }

    private void UpdatePreviewBitmap()
    {
        if (SurfaceState is null)
        {
            BgraPreviewHelper.ClearPreview(PreviewImage);
            _lastPreviewSurfaceKey = null;
            PlaceholderPanel.Visibility = Visibility.Visible;
            ScheduleSourceFramingRefresh();
            return;
        }

        if (HasMediaAssetPreview)
        {
            BgraPreviewHelper.ClearPreview(PreviewImage);
            PreviewImage.Visibility = Visibility.Collapsed;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            ScheduleSourceFramingRefresh();
            return;
        }

        if (IsGpuPathActive)
        {
            // The GPU swap chain is presenting real frames (e.g. a capture input).
            // Hide the BGRA preview and the "waiting / choose a source" placeholder so
            // they don't paint on top of the live video.
            PreviewImage.Visibility = Visibility.Collapsed;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            ScheduleSourceFramingRefresh();
            return;
        }

        if (!string.Equals(_lastPreviewSurfaceKey, SurfaceState.SurfaceKey, StringComparison.Ordinal))
        {
            BgraPreviewHelper.ClearPreview(PreviewImage);
            _lastPreviewSurfaceKey = SurfaceState.SurfaceKey;
        }

        BgraPreviewHelper.SetPreview(
            PreviewImage,
            SurfaceState.PreviewBgra,
            SurfaceState.PreviewWidth,
            SurfaceState.PreviewHeight);

        if (SurfaceState.HasPreviewBitmap || BgraPreviewHelper.HasPresentedFrame(PreviewImage))
        {
            PreviewImage.Visibility = Visibility.Visible;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            ScheduleSourceFramingRefresh();
            return;
        }

        PreviewImage.Visibility = Visibility.Collapsed;
        PlaceholderPanel.Visibility = Visibility.Visible;
        ScheduleSourceFramingRefresh();
    }

    private void ApplySourceFit()
    {
        ApplySourceFraming();
    }

    private void ApplySourceFraming()
    {
        var viewportWidth = SurfaceViewport.ActualWidth > 0 ? SurfaceViewport.ActualWidth : ActualWidth;
        var viewportHeight = SurfaceViewport.ActualHeight > 0 ? SurfaceViewport.ActualHeight : ActualHeight;
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return;
        }

        SurfaceViewport.Clip = new RectangleGeometry
        {
            Rect = new Rect(0, 0, viewportWidth, viewportHeight)
        };

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
        var transform = new TranslateTransform
        {
            X = layout.TranslateX,
            Y = layout.TranslateY
        };
        PreviewImage.RenderTransform = transform;
        MediaPreview.Width = layout.Width;
        MediaPreview.Height = layout.Height;
        MediaPreview.RenderTransform = new TranslateTransform
        {
            X = layout.TranslateX,
            Y = layout.TranslateY
        };
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

    private void RefreshPathBindings()
    {
        if (_refreshingPathBindings)
        {
            return;
        }

        _refreshingPathBindings = true;
        try
        {
            Bindings.Update();
        }
        finally
        {
            _refreshingPathBindings = false;
        }
    }
}
