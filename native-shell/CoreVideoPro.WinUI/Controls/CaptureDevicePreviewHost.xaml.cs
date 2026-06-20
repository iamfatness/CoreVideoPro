using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class CaptureDevicePreviewHost : UserControl
{
    public static readonly DependencyProperty DeviceIdProperty =
        DependencyProperty.Register(
            nameof(DeviceId),
            typeof(string),
            typeof(CaptureDevicePreviewHost),
            new PropertyMetadata(null, OnPreviewPropertyChanged));

    public static readonly DependencyProperty IsOnlineProperty =
        DependencyProperty.Register(
            nameof(IsOnline),
            typeof(bool),
            typeof(CaptureDevicePreviewHost),
            new PropertyMetadata(false, OnPreviewPropertyChanged));

    private MediaCapture? _capture;
    private MediaFrameReader? _reader;
    private string? _activeDeviceId;
    private bool _paintingFrame;

    public CaptureDevicePreviewHost()
    {
        InitializeComponent();
        Unloaded += OnUnloaded;
    }

    public string? DeviceId
    {
        get => (string?)GetValue(DeviceIdProperty);
        set => SetValue(DeviceIdProperty, value);
    }

    public bool IsOnline
    {
        get => (bool)GetValue(IsOnlineProperty);
        set => SetValue(IsOnlineProperty, value);
    }

    public string StatusTitle { get; private set; } = "Camera preview idle";

    public string StatusDetail { get; private set; } = "Bring this device online to open a local UVC preview.";

    private static void OnPreviewPropertyChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is CaptureDevicePreviewHost host)
        {
            _ = host.RefreshPreviewAsync();
        }
    }

    private async Task RefreshPreviewAsync()
    {
        if (!IsOnline || string.IsNullOrWhiteSpace(DeviceId))
        {
            await StopPreviewAsync();
            SetStatus("Camera preview idle", "Bring this device online to open a local UVC preview.");
            return;
        }

        if (_capture is not null && string.Equals(_activeDeviceId, DeviceId, StringComparison.Ordinal))
        {
            return;
        }

        await StopPreviewAsync();
        SetStatus("Opening camera", "Requesting the selected Windows video capture device.");

        try
        {
            _capture = new MediaCapture();
            await _capture.InitializeAsync(new MediaCaptureInitializationSettings
            {
                VideoDeviceId = DeviceId,
                StreamingCaptureMode = StreamingCaptureMode.Video
            });

            var source = _capture.FrameSources.Values.FirstOrDefault(frameSource =>
                frameSource.Info.MediaStreamType == MediaStreamType.VideoPreview &&
                frameSource.Info.SourceKind == MediaFrameSourceKind.Color) ??
                _capture.FrameSources.Values.FirstOrDefault(frameSource =>
                    frameSource.Info.SourceKind == MediaFrameSourceKind.Color);

            if (source is null)
            {
                throw new InvalidOperationException("No color video frame source was exposed by this device.");
            }

            _reader = await _capture.CreateFrameReaderAsync(source, MediaEncodingSubtypes.Bgra8);
            _reader.FrameArrived += OnFrameArrived;
            var status = await _reader.StartAsync();
            if (status != MediaFrameReaderStartStatus.Success)
            {
                throw new InvalidOperationException($"Frame reader did not start: {status}");
            }

            _activeDeviceId = DeviceId;
            PreviewImage.Visibility = Visibility.Visible;
            StatusPanel.Visibility = Visibility.Collapsed;
        }
        catch (Exception ex)
        {
            await StopPreviewAsync();
            SetStatus("Camera preview failed", ex.Message);
        }
    }

    private async Task StopPreviewAsync()
    {
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewImage.Source = null;
        _activeDeviceId = null;

        if (_reader is not null)
        {
            _reader.FrameArrived -= OnFrameArrived;
            try
            {
                await _reader.StopAsync();
            }
            catch
            {
                // StopAsync can throw after a failed device initialization.
            }

            _reader.Dispose();
            _reader = null;
        }

        if (_capture is null)
        {
            return;
        }

        _capture.Dispose();
        _capture = null;
    }

    private void OnFrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
    {
        if (_paintingFrame)
        {
            return;
        }

        using var frame = sender.TryAcquireLatestFrame();
        var bitmap = frame?.VideoMediaFrame?.SoftwareBitmap;
        if (bitmap is null)
        {
            return;
        }

        SoftwareBitmap displayBitmap;
        if (bitmap.BitmapPixelFormat == BitmapPixelFormat.Bgra8 &&
            bitmap.BitmapAlphaMode == BitmapAlphaMode.Premultiplied)
        {
            displayBitmap = SoftwareBitmap.Copy(bitmap);
        }
        else
        {
            displayBitmap = SoftwareBitmap.Convert(bitmap, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied);
        }

        _paintingFrame = true;
        DispatcherQueue.TryEnqueue(async () =>
        {
            try
            {
                var source = new SoftwareBitmapSource();
                await source.SetBitmapAsync(displayBitmap);
                PreviewImage.Source = source;
            }
            finally
            {
                displayBitmap.Dispose();
                _paintingFrame = false;
            }
        });
    }

    private void SetStatus(string title, string detail)
    {
        StatusTitle = title;
        StatusDetail = detail;
        StatusPanel.Visibility = Visibility.Visible;
        Bindings.Update();
    }

    private async void OnUnloaded(object sender, RoutedEventArgs e) => await StopPreviewAsync();
}
