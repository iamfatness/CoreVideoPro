using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class VideoSurfaceHost : UserControl, IVideoSurfacePresenter, IDirect3DVideoSurfaceHost
{
    public static readonly DependencyProperty SurfaceStateProperty =
        DependencyProperty.Register(
            nameof(SurfaceState),
            typeof(VideoSurfaceState),
            typeof(VideoSurfaceHost),
            new PropertyMetadata(null, OnSurfaceStateChanged));

    private readonly Direct3D11InteropService _direct3DInterop = new();
    private nint _direct3DDevicePointer;

    public VideoSurfaceHost()
    {
        InitializeComponent();
        _direct3DInterop.PresentationPathChanged += OnPresentationPathChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    public VideoSurfaceState? SurfaceState
    {
        get => (VideoSurfaceState?)GetValue(SurfaceStateProperty);
        set => SetValue(SurfaceStateProperty, value);
    }

    public string SurfaceKey => SurfaceState?.SurfaceKey ?? "unknown";

    public string Title => SurfaceState?.Title ?? "Video";

    public string StatusLine => SurfaceState?.StatusLine ?? "Waiting for frames";

    public string DetailLine => SurfaceState?.DetailLine ?? "Start the engine to receive media-core frames.";

    public bool HasFrameStats => SurfaceState?.LastFrame is not null;

    public string ParticipantLine =>
        SurfaceState?.LastFrame?.ParticipantId is { Length: > 0 } participantId
            ? $"Participant {participantId}"
            : SurfaceState?.LastFrame?.RenderPlanId is { Length: > 0 } renderPlanId
                ? $"Render plan {renderPlanId}"
                : "Compositor output";

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
        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
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
            host.RefreshPathBindings();
        }
    }

    private bool IsProgramSurface =>
        string.Equals(SurfaceKey, "program", StringComparison.Ordinal) ||
        SurfaceState?.Kind == VideoSurfaceKind.Program;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
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
            BgraPreviewHelper.SetPreview(PreviewImage, null, 0, 0);
            PlaceholderPanel.Visibility = Visibility.Visible;
            return;
        }

        if (IsGpuPathActive)
        {
            return;
        }

        BgraPreviewHelper.SetPreview(
            PreviewImage,
            SurfaceState.PreviewBgra,
            SurfaceState.PreviewWidth,
            SurfaceState.PreviewHeight);

        if (SurfaceState.HasPreviewBitmap)
        {
            PreviewImage.Visibility = Visibility.Visible;
            PlaceholderPanel.Visibility = Visibility.Collapsed;
            return;
        }

        PreviewImage.Visibility = Visibility.Collapsed;
        PlaceholderPanel.Visibility = Visibility.Visible;
    }

    private void RefreshPathBindings() => Bindings.Update();
}