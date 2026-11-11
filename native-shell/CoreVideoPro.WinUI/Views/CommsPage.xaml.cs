using System;
using System.Collections.Generic;
using System.Windows.Input;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.ViewModels.Comms;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using Windows.UI.Core;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Comms page (docs/reference/comms-talkback.md). Panel-scoped keys (ZComms model): digits
/// 1-9 key the first nine reachable cells, Space = ALL CALL, both momentary (or toggles in
/// Latch). Never global: only while focus is inside this page, never while typing in a
/// field, never with a modifier, key auto-repeat ignored; every held key goes up when focus
/// leaves the page, the window deactivates (MainWindow) or the operator navigates away
/// (StudioViewModel).
/// </summary>
public sealed partial class CommsPage : UserControl
{
    private readonly HashSet<VirtualKey> _down = new();

    public CommsPage()
    {
        InitializeComponent();
        PreviewKeyDown += OnPreviewKeyDown;
        PreviewKeyUp += OnPreviewKeyUp;
        LosingFocus += OnLosingFocus;
    }

    public CommsViewModel? ViewModel
    {
        get => (CommsViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(nameof(ViewModel), typeof(CommsViewModel), typeof(CommsPage), new PropertyMetadata(null));

    /// <summary>The existing Zoom Leave (Settings page); Comms never leaves on its own.</summary>
    public ICommand? LeaveCommand
    {
        get => (ICommand?)GetValue(LeaveCommandProperty);
        set => SetValue(LeaveCommandProperty, value);
    }

    public static readonly DependencyProperty LeaveCommandProperty =
        DependencyProperty.Register(nameof(LeaveCommand), typeof(ICommand), typeof(CommsPage), new PropertyMetadata(null));

    // x:Bind helpers.
    public static string OnOff(bool on) => on ? "on" : "off";
    public static double SegmentOpacity(bool lit) => lit ? 1.0 : 0.18;

    /// <summary>Called by the workspace when the Comms tab becomes visible, so the digit and
    /// Space keys work without a click first (our own window only; never another app).</summary>
    public void FocusDesk() => Focus(FocusState.Programmatic);

    // ------------------------------------------------------------ pointer keys

    private void OnCellPressed(object sender, EventArgs e)
    {
        if (sender is TalkKey { Tag: CommsCellViewModel cell }) _ = ViewModel?.PressAsync(cell);
    }

    private void OnCellReleased(object sender, EventArgs e)
    {
        if (sender is TalkKey { Tag: CommsCellViewModel cell }) _ = ViewModel?.ReleaseAsync(cell);
    }

    private void OnAllPressed(object sender, EventArgs e) => _ = ViewModel?.AllPressAsync();
    private void OnAllReleased(object sender, EventArgs e) => _ = ViewModel?.AllReleaseAsync();

    // ------------------------------------------------------------ pickers

    private void OnMicChosen(object sender, SelectionChangedEventArgs e)
    {
        if (ViewModel is { } vm && MicPicker.SelectedItem is string device) vm.SelectedMic = device;
    }

    private void OnOutputChosen(object sender, SelectionChangedEventArgs e)
    {
        if (ViewModel is { } vm && OutputPicker.SelectedItem is string device) vm.SelectedOutput = device;
    }

    // ------------------------------------------------------------ keyboard

    private static bool Typing(object? source) =>
        source is TextBox or ComboBox or ComboBoxItem or PasswordBox or AutoSuggestBox or NumberBox or RichEditBox;

    private static bool ModifierDown()
    {
        static bool Down(VirtualKey k) =>
            InputKeyboardSource.GetKeyStateForCurrentThread(k).HasFlag(CoreVirtualKeyStates.Down);
        return Down(VirtualKey.Control) || Down(VirtualKey.Menu) || Down(VirtualKey.Shift) ||
               Down(VirtualKey.LeftWindows) || Down(VirtualKey.RightWindows);
    }

    private static int Digit(VirtualKey key) =>
        key is >= VirtualKey.Number1 and <= VirtualKey.Number9 ? key - VirtualKey.Number0
        : key is >= VirtualKey.NumberPad1 and <= VirtualKey.NumberPad9 ? key - VirtualKey.NumberPad0 : 0;

    private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (ViewModel is not { DeskVisible: true } vm || Typing(e.OriginalSource) || ModifierDown()) return;
        var digit = Digit(e.Key);
        if (e.Key != VirtualKey.Space && digit == 0) return;
        e.Handled = true;
        if (!_down.Add(e.Key)) return;  // auto-repeat must not re-toggle a latch
        if (e.Key == VirtualKey.Space)
        {
            _ = vm.AllPressAsync();
            return;
        }
        if (vm.CellForDigit(digit) is { } cell) _ = vm.PressAsync(cell);
    }

    private void OnPreviewKeyUp(object sender, KeyRoutedEventArgs e)
    {
        var wasDown = _down.Remove(e.Key);
        if (ViewModel is not { } vm || !wasDown) return;
        if (e.Key == VirtualKey.Space)
        {
            e.Handled = true;
            _ = vm.AllReleaseAsync();
            return;
        }
        if (Digit(e.Key) is var d and > 0 && vm.CellForDigit(d) is { } cell)
        {
            e.Handled = true;
            _ = vm.ReleaseAsync(cell);
        }
    }

    // Focus moving out of the page releases everything (it never moves on its own while
    // a key is held by the pointer: the TalkKey keeps capture).
    private void OnLosingFocus(UIElement sender, LosingFocusEventArgs args)
    {
        if (args.NewFocusedElement is DependencyObject next && IsInside(next)) return;
        ReleaseEverything();
    }

    private bool IsInside(DependencyObject element)
    {
        for (var node = element; node is not null; node = Microsoft.UI.Xaml.Media.VisualTreeHelper.GetParent(node))
            if (ReferenceEquals(node, this)) return true;
        return false;
    }

    /// <summary>Every held key up (focus left, window deactivated, page hidden).</summary>
    public void ReleaseEverything()
    {
        _down.Clear();
        AllCallKey.ForceUp();
        _ = ViewModel?.ReleaseAllAsync();
    }
}
