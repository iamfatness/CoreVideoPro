using CoreVideoPro.MediaCore.Models;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Stable curve geometry, edited at pointer rate only; no video pixels.</summary>
public sealed class GradeCurveControl : UserControl
{
    private readonly Canvas _canvas = new() { Background = new SolidColorBrush(ColorHelper.FromArgb(255, 24, 25, 28)) };
    private readonly Polyline _line = new() { Stroke = new SolidColorBrush(Colors.White), StrokeThickness = 2 };
    private readonly Ellipse[] _dots = Enumerable.Range(0, 16).Select(_ => new Ellipse {
        Width = 10, Height = 10, Fill = new SolidColorBrush(Colors.White), IsHitTestVisible = false }).ToArray();
    private GradeCurvePoint[] _points = [new(0,0),new(1,1)];
    private bool _dragging;
    public int SelectedIndex { get; private set; } = -1;
    public IReadOnlyList<GradeCurvePoint> Points => _points;
    public event EventHandler? CurveEdited;
    public event EventHandler? PointSelected;
    public GradeCurveControl()
    {
        Content = _canvas; MinHeight = 220; IsTabStop = true;
        AutomationProperties.SetName(this,"Grade curve");
        AutomationProperties.SetHelpText(this,"Home or End selects an endpoint. Control plus left or right selects a point. Arrow keys adjust it; Shift makes a larger adjustment. Delete removes an interior point.");
        for (var i = 1; i < 4; ++i) {
            var vertical = new Line { Stroke = new SolidColorBrush(ColorHelper.FromArgb(255,48,49,54)), StrokeThickness = 1 };
            var horizontal = new Line { Stroke = vertical.Stroke, StrokeThickness = 1 };
            _canvas.Children.Add(vertical); _canvas.Children.Add(horizontal);
            var fraction = i / 4.0;
            SizeChanged += (_, _) => { vertical.X1 = vertical.X2 = ActualWidth * fraction; vertical.Y2 = ActualHeight;
                horizontal.Y1 = horizontal.Y2 = ActualHeight * fraction; horizontal.X2 = ActualWidth; };
        }
        _canvas.Children.Add(_line); foreach (var dot in _dots) _canvas.Children.Add(dot);
        SizeChanged += (_, _) => Draw();
        PointerPressed += OnPressed; PointerMoved += OnMoved;
        PointerReleased += (_, e) => { _dragging = false; ReleasePointerCapture(e.Pointer); };
        PointerCaptureLost += (_, _) => _dragging = false;
        DoubleTapped += (_, e) => { if (SelectedIndex > 0 && SelectedIndex < _points.Length-1) RemoveSelected(); e.Handled = true; };
        KeyDown += (_, e) => e.Handled=HandleEditingKey(e.Key,
            Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down),
            Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down));
    }
    internal bool HandleEditingKey(VirtualKey key,bool shift=false,bool control=false)
    {
        if (!IsEnabled) return false;
        if (key is VirtualKey.Home or VirtualKey.End || (control && key is (VirtualKey.Left or VirtualKey.Right)))
        {
            SelectedIndex=key==VirtualKey.Home?0:key==VirtualKey.End?_points.Length-1:
                Math.Clamp(SelectedIndex+(key==VirtualKey.Left?-1:1),0,_points.Length-1);
            Draw();PointSelected?.Invoke(this,EventArgs.Empty);return true;
        }
        if (key is VirtualKey.Delete or VirtualKey.Back) {RemoveSelected();return true;}
        if (key is not (VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down)) return false;
        if (SelectedIndex<0) {SelectedIndex=0;Draw();PointSelected?.Invoke(this,EventArgs.Empty);return true;}
        var point=_points[SelectedIndex];var step=shift ? .01 : .001;
        SetSelected(point.X+(key==VirtualKey.Left?-step:key==VirtualKey.Right?step:0),
            point.Y+(key==VirtualKey.Down?-step:key==VirtualKey.Up?step:0));return true;
    }
    public void SetPoints(IReadOnlyList<GradeCurvePoint> points)
    {
        _points = points.ToArray(); if (SelectedIndex >= _points.Length) SelectedIndex = -1; Draw(); PointSelected?.Invoke(this,EventArgs.Empty);
    }
    public void SetSelected(double x, double y)
    {
        if (SelectedIndex < 0 || !double.IsFinite(x) || !double.IsFinite(y)) return;
        var i = SelectedIndex;
        if (i==0) x=0; else if (i==_points.Length-1) x=1; else {
            var gap=Math.Min((_points[i+1].X-_points[i-1].X)/4,.000001);
            x=Math.Clamp(x,_points[i-1].X+gap,_points[i+1].X-gap);
        }
        _points[i] = new(x,Math.Clamp(y,0,1)); Draw(); CurveEdited?.Invoke(this,EventArgs.Empty); PointSelected?.Invoke(this,EventArgs.Empty);
    }
    public void Reset() { SelectedIndex = -1; _points = [new(0,0),new(1,1)]; Draw(); CurveEdited?.Invoke(this,EventArgs.Empty); PointSelected?.Invoke(this,EventArgs.Empty); }
    private void RemoveSelected()
    {
        if (SelectedIndex <= 0 || SelectedIndex >= _points.Length-1) return;
        _points = _points.Where((_,i) => i != SelectedIndex).ToArray(); SelectedIndex = -1;
        Draw(); CurveEdited?.Invoke(this,EventArgs.Empty); PointSelected?.Invoke(this,EventArgs.Empty);
    }
    private void OnPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!IsEnabled || ActualWidth <= 0 || ActualHeight <= 0) return;
        Focus(FocusState.Pointer); var pos = e.GetCurrentPoint(this).Position;
        var closest = -1; var distance = 13.0;
        for (var i = 0; i < _points.Length; ++i) {
            var dx = pos.X-_points[i].X*ActualWidth; var dy = pos.Y-(1-_points[i].Y)*ActualHeight;
            var d = Math.Sqrt(dx*dx+dy*dy); if (d < distance) { closest = i; distance = d; }
        }
        if (closest < 0 && _points.Length < 16) {
            var x = Math.Clamp(pos.X/ActualWidth,.001,.999); var y = Math.Clamp(1-pos.Y/ActualHeight,0,1);
            if (_points.All(p => Math.Abs(p.X-x) >= .002)) {
                _points = _points.Append(new(x,y)).OrderBy(p => p.X).ToArray(); closest = Array.FindIndex(_points,p => p.X == x);
                CurveEdited?.Invoke(this,EventArgs.Empty);
            }
        }
        SelectedIndex = closest; _dragging = closest >= 0;
        if (_dragging) CapturePointer(e.Pointer); Draw(); PointSelected?.Invoke(this,EventArgs.Empty); e.Handled = true;
    }
    private void OnMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_dragging || ActualWidth <= 0 || ActualHeight <= 0) return;
        var pos = e.GetCurrentPoint(this).Position; SetSelected(pos.X/ActualWidth,1-pos.Y/ActualHeight);
        PointSelected?.Invoke(this,EventArgs.Empty); e.Handled = true;
    }
    private void Draw()
    {
        _line.Points = new PointCollection();
        foreach (var point in _points) _line.Points.Add(new Point(point.X*ActualWidth,(1-point.Y)*ActualHeight));
        for (var i = 0; i < _dots.Length; ++i) {
            var dot = _dots[i]; dot.Visibility = i < _points.Length ? Visibility.Visible : Visibility.Collapsed;
            if (i >= _points.Length) continue;
            dot.Fill = new SolidColorBrush(i == SelectedIndex ? Colors.DeepSkyBlue : Colors.White);
            Canvas.SetLeft(dot,_points[i].X*ActualWidth-5); Canvas.SetTop(dot,(1-_points[i].Y)*ActualHeight-5);
        }
    }
}
