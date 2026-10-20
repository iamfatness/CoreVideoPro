using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

// TextChanged arrives after rendering. Track intent synchronously so a model
// refresh cannot erase it in that gap; keep model/visual-tree work out of
// TextChanging itself. Each inspector owns and retires its own bindings.
internal sealed class GraphicsTextFieldBinding : IDisposable
{
    private readonly TextBox _text;
    private readonly Func<string?> _read;
    private readonly Func<string, bool> _write;
    private readonly Action _beginGesture, _endGesture;
    private bool _refreshing, _pending, _disposed, _gestureActive, _commitQueued;

    internal GraphicsTextFieldBinding(TextBox text, Func<string?> read, Func<string, bool> write, Action beginGesture, Action endGesture)
    {
        _text = text; _read = read; _write = write; _beginGesture = beginGesture; _endGesture = endGesture;
        text.TextChanging += OnTextChanging;
        text.TextChanged += OnTextChanged;
        text.LostFocus += OnLostFocus;
        text.GotFocus += OnGotFocus;
    }

    private void OnTextChanging(TextBox sender, TextBoxTextChangingEventArgs args)
    {
        if (!_disposed && !_refreshing) _pending = true;
    }

    private void OnTextChanged(object sender, TextChangedEventArgs args) => CommitPending();
    private void OnGotFocus(object sender, RoutedEventArgs args) { if (!_disposed && !_gestureActive) { _gestureActive = true; _beginGesture(); } }
    private void OnLostFocus(object sender, RoutedEventArgs args)
    {
        if (_disposed) return;
        CommitPending();
        if (_gestureActive) { _gestureActive = false; _endGesture(); }
    }

    internal void CommitPending()
    {
        if (_disposed || !_pending) return;
        _pending = false;
        if (!_write(_text.Text)) _pending = true;
    }

    internal void Refresh()
    {
        if (_disposed) return;
        if (_pending) {
            // Retry after a busy session becomes editable, outside Refresh's
            // visual-tree traversal. A rejected edit remains local intent.
            if (!_commitQueued) {
                _commitQueued = _text.DispatcherQueue.TryEnqueue(() => { _commitQueued = false; CommitPending(); });
            }
            return;
        }
        if (_text.FocusState != FocusState.Unfocused || _read() is not { } value || value == _text.Text) return;
        _refreshing = true;
        try { _text.Text = value; }
        finally { _refreshing = false; }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true; _pending = false;
        if (_gestureActive) { _gestureActive = false; _endGesture(); }
        _text.TextChanging -= OnTextChanging;
        _text.TextChanged -= OnTextChanged;
        _text.LostFocus -= OnLostFocus;
        _text.GotFocus -= OnGotFocus;
    }
}
