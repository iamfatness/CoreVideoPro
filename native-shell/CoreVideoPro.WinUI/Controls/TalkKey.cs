using System;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// A talk key (Comms page): press = key down, release (or losing the pointer) = key up.
/// A Button's Click fires on release, which is the opposite of what a talk key needs, so
/// the pointer is handled here (with capture, so dragging off the key still releases it)
/// and Click/Command are never used. Space and Enter are left to bubble: Space is ALL CALL
/// on the Comms page (ZComms model), and a focused key must not swallow it. Same contract
/// as CoreVideo Edge's TalkKey; styled as an ordinary Pro button.
/// </summary>
public sealed class TalkKey : Button
{
    private bool _down;
    private uint? _pointerId;

    public TalkKey()
    {
        DefaultStyleKey = typeof(Button);
    }

    /// <summary>Raised once per press (never repeated while held).</summary>
    public event EventHandler? Pressed;

    /// <summary>Raised once per release, including capture loss and cancel.</summary>
    public event EventHandler? Released;

    /// <summary>True between Pressed and Released.</summary>
    public bool IsDown => _down;

    protected override void OnPointerPressed(PointerRoutedEventArgs e)
    {
        e.Handled = true;
        if (!IsEnabled) return;
        var point = e.GetCurrentPoint(this);
        if (e.Pointer.PointerDeviceType == PointerDeviceType.Mouse && !point.Properties.IsLeftButtonPressed) return;
        if (_down) return;
        if (CapturePointer(e.Pointer)) _pointerId = e.Pointer.PointerId;
        _down = true;
        Pressed?.Invoke(this, EventArgs.Empty);
    }

    protected override void OnPointerReleased(PointerRoutedEventArgs e)
    {
        e.Handled = true;
        Up();
        if (_pointerId == e.Pointer.PointerId) ReleasePointerCapture(e.Pointer);
        _pointerId = null;
    }

    protected override void OnPointerCaptureLost(PointerRoutedEventArgs e)
    {
        _pointerId = null;
        Up();
    }

    protected override void OnPointerCanceled(PointerRoutedEventArgs e)
    {
        _pointerId = null;
        Up();
    }

    // No pointer-over / pressed visual states: the key's colour is the engine's truth
    // (keyed, listeners), never the template's hover or pressed brush.
    protected override void OnPointerEntered(PointerRoutedEventArgs e) => e.Handled = true;
    protected override void OnPointerExited(PointerRoutedEventArgs e) => e.Handled = true;

    protected override void OnKeyDown(KeyRoutedEventArgs e)
    {
        if (e.Key is VirtualKey.Space or VirtualKey.Enter) return;  // bubble to the page
        base.OnKeyDown(e);
    }

    protected override void OnKeyUp(KeyRoutedEventArgs e)
    {
        if (e.Key is VirtualKey.Space or VirtualKey.Enter) return;
        base.OnKeyUp(e);
    }

    /// <summary>Forces the key up (the page releases everything on deactivate).</summary>
    public void ForceUp() => Up();

    private void Up()
    {
        if (!_down) return;
        _down = false;
        Released?.Invoke(this, EventArgs.Empty);
    }
}
