using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Pop-out audio mixer control surface. Real audio routing and DSP remain in the media engine.
/// </summary>
public sealed partial class AudioMixerWindow : Window
{
    private const int WindowWidth = 1080;
    private const int WindowHeight = 700;
    private bool _ready;

    public AudioMixerWindow(StudioViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        InitializeComponent();
        Closed += OnWindowClosed;
        ApplyChromeAndSize();
        ShowRotaryStyle();
        _ready = true;
    }

    public StudioViewModel ViewModel { get; }

    public event EventHandler? WindowClosed;

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        Closed -= OnWindowClosed;
        WindowClosed?.Invoke(this, EventArgs.Empty);
    }

    private void OnRotaryStyleClicked(object sender, RoutedEventArgs args) => ShowRotaryStyle();

    private void OnDawStyleClicked(object sender, RoutedEventArgs args) => ShowDawStyle();

    private void ShowRotaryStyle()
    {
        RotaryMixerPanel.Visibility = Visibility.Visible;
        DawMixerPanel.Visibility = Visibility.Collapsed;
        RotaryStyleButton.IsEnabled = false;
        DawStyleButton.IsEnabled = true;
    }

    private void ShowDawStyle()
    {
        RotaryMixerPanel.Visibility = Visibility.Collapsed;
        DawMixerPanel.Visibility = Visibility.Visible;
        RotaryStyleButton.IsEnabled = true;
        DawStyleButton.IsEnabled = false;
    }

    private void OnSelectClicked(object sender, RoutedEventArgs args)
    {
        if (TryResolveParticipantId(sender, out var participantId))
        {
            ViewModel.SelectParticipantCommand.Execute(participantId);
        }
    }

    private void OnMuteClicked(object sender, RoutedEventArgs args)
    {
        if (TryResolveParticipantId(sender, out var participantId))
        {
            ViewModel.ToggleMixerMute(participantId);
        }
    }

    private void OnGainSliderValueChanged(object sender, RangeBaseValueChangedEventArgs args)
    {
        if (!_ready || !TryResolveParticipantId(sender, out var participantId))
        {
            return;
        }

        ViewModel.SetMixerManualGain(participantId, args.NewValue);
    }

    private void OnPanSliderValueChanged(object sender, RangeBaseValueChangedEventArgs args)
    {
        if (!_ready || !TryResolveParticipantId(sender, out var participantId))
        {
            return;
        }

        ViewModel.SetMixerPan(participantId, args.NewValue);
    }

    private static bool TryResolveParticipantId(object sender, out string participantId)
    {
        participantId = string.Empty;
        if (sender is not FrameworkElement { Tag: string { Length: > 0 } tag })
        {
            return false;
        }

        participantId = tag;
        return true;
    }

    private void ApplyChromeAndSize()
    {
        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        if (appWindow is null)
        {
            return;
        }

        appWindow.Resize(new Windows.Graphics.SizeInt32(WindowWidth, WindowHeight));
        WindowChromeService.Apply(this, appWindow);

        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsMaximizable = true;
            presenter.IsMinimizable = true;
            presenter.IsResizable = true;
        }
    }
}
