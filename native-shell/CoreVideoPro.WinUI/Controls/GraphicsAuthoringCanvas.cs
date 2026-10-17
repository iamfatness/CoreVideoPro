using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Editor guides and pointer input only. Every video/graphic pixel is
/// still supplied by the native private monitor through VideoSurfaceHost.</summary>
public sealed class GraphicsAuthoringCanvas : UserControl
{
    private readonly Grid _root=new(),_viewport=new();
    private readonly VideoSurfaceHost _video=new();
    private readonly Canvas _guides=new() { Background=new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
    private readonly Rectangle _safe=new() { Stroke=new SolidColorBrush(Windows.UI.Color.FromArgb(140,180,190,205)),StrokeThickness=1,IsHitTestVisible=false,StrokeDashArray=new DoubleCollection { 4,4 } };
    private readonly Rectangle _selection=new() { Stroke=new SolidColorBrush(Microsoft.UI.Colors.DeepSkyBlue),StrokeThickness=2,IsHitTestVisible=false };
    private readonly Rectangle _handle=new() { Width=10,Height=10,Fill=new SolidColorBrush(Microsoft.UI.Colors.DeepSkyBlue),IsHitTestVisible=false };
    private GraphicsWorkspaceSession? _session;
    private Point _start; private NativeGraphicsPlacement? _before; private bool _resize;
    private double _width=1050,_height=176;
    public VideoSurfaceState? SurfaceState { get=>_video.SurfaceState; set=>_video.SurfaceState=value; }
    public GraphicsWorkspaceSession? Session
    {
        get=>_session;
        set { Cancel(); if(_session is not null) _session.Changed-=Changed; _session=value; if(_session is not null) _session.Changed+=Changed; Draw(); }
    }
    public GraphicsAuthoringCanvas()
    {
        IsTabStop=true;
        _viewport.HorizontalAlignment=HorizontalAlignment.Center; _viewport.VerticalAlignment=VerticalAlignment.Center;
        _viewport.Children.Add(_video); _viewport.Children.Add(_guides); _root.Children.Add(_viewport); Content=_root;
        _guides.Children.Add(_safe); _guides.Children.Add(_selection); _guides.Children.Add(_handle);
        SizeChanged+=(_,_)=>Draw(); _guides.PointerPressed+=Pressed; _guides.PointerMoved+=Moved;
        _guides.PointerReleased+=Released; _guides.PointerCaptureLost+=(_,_)=>Cancel(); KeyDown+=Key;
        AutomationProperties.SetName(this,"Graphics native canvas. Drag to move; drag the corner to scale. Arrow keys nudge; Shift nudges ten pixels; Escape cancels.");
    }
    private void Changed(object? sender,EventArgs e)=>Draw();
    private NativeGraphicsPlacement Placement()
    {
        if(_session?.Draft is not { } draft) return new();
        return draft.Placement ?? new() { Anchor=draft.Style.Anchor,
            X=draft.Style.SafeOffsetX*1920*(draft.Style.Anchor.Contains("right",StringComparison.Ordinal)?-1:1),
            Y=draft.Style.SafeOffsetY*1080*(draft.Style.Anchor.Contains("upper",StringComparison.Ordinal)?1:-1) };
    }
    private static (double X,double Y) Anchor(NativeGraphicsPlacement p)=>
        (p.Anchor.Contains("right",StringComparison.Ordinal)?1:p.Anchor.Contains("left",StringComparison.Ordinal)?0:.5,
         p.Anchor.Contains("upper",StringComparison.Ordinal)?0:p.Anchor.Contains("lower",StringComparison.Ordinal)?1:.5);
    private Rect Bounds(NativeGraphicsPlacement p)
    {
        var anchor=Anchor(p); return new(anchor.X*(1920-_width*p.Scale)+p.X,anchor.Y*(1080-_height*p.Scale)+p.Y,_width*p.Scale,_height*p.Scale);
    }
    private void Draw()
    {
        var fit=Math.Min(ActualWidth/1920,ActualHeight/1080); if(fit<=0 || !double.IsFinite(fit)) return;
        _viewport.Width=1920*fit; _viewport.Height=1080*fit;
        _safe.Width=1728*fit; _safe.Height=972*fit; Canvas.SetLeft(_safe,96*fit); Canvas.SetTop(_safe,54*fit);
        var draft=_session?.Draft; _selection.Visibility=_handle.Visibility=draft is null ? Visibility.Collapsed : Visibility.Visible;
        if(draft is null) return;
        if(_session?.Template?.Bounds is { } bounds) { _width=bounds.GetProperty("width").GetDouble(); _height=bounds.GetProperty("height").GetDouble(); }
        else { _width=Math.Min(draft.Style.Width,1-draft.Style.SafeOffsetX)*1920; _height=Math.Min(.4,(draft.Style.NameSize+draft.Style.TitleSize+draft.Style.Padding*3)/1080)*1080; }
        var rect=Bounds(Placement()); _selection.Width=rect.Width*fit; _selection.Height=rect.Height*fit;
        Canvas.SetLeft(_selection,rect.X*fit); Canvas.SetTop(_selection,rect.Y*fit);
        Canvas.SetLeft(_handle,rect.Right*fit-5); Canvas.SetTop(_handle,rect.Bottom*fit-5);
    }
    private void Pressed(object sender,PointerRoutedEventArgs e)
    {
        if(_session?.Draft is null || _session.Busy || !e.GetCurrentPoint(_guides).Properties.IsLeftButtonPressed) return;
        Focus(FocusState.Pointer); var point=e.GetCurrentPoint(_guides).Position; var fit=_viewport.Width/1920;
        var rect=Bounds(Placement()); var reference=new Point(point.X/fit,point.Y/fit);
        _resize=Math.Abs(reference.X-rect.Right)*fit<14 && Math.Abs(reference.Y-rect.Bottom)*fit<14;
        if(!_resize && !rect.Contains(reference)) return;
        _start=reference; _before=Placement(); _session.BeginGesture(); if(!_guides.CapturePointer(e.Pointer)) Cancel(); e.Handled=true;
    }
    private void Moved(object sender,PointerRoutedEventArgs e)
    {
        if(_before is not { } before || _session is null) return;
        var point=e.GetCurrentPoint(_guides).Position; var fit=_viewport.Width/1920;
        var dx=point.X/fit-_start.X; var dy=point.Y/fit-_start.Y;
        if(!_resize) _session.SetPlacement(before with { X=Math.Clamp(before.X+dx,-3840,3840),Y=Math.Clamp(before.Y+dy,-2160,2160) });
        else {
            var scale=Math.Clamp(before.Scale+dx/Math.Max(_width,1),.1,4); var anchor=Anchor(before);
            _session.SetPlacement(before with { Scale=scale,X=Math.Clamp(before.X+anchor.X*_width*(scale-before.Scale),-3840,3840),Y=Math.Clamp(before.Y+anchor.Y*_height*(scale-before.Scale),-2160,2160) });
        }
        e.Handled=true;
    }
    private void Released(object sender,PointerRoutedEventArgs e) { if(_before is null) return; _before=null; _session?.EndGesture(); _guides.ReleasePointerCapture(e.Pointer); e.Handled=true; }
    private void Cancel() { if(_before is null) return; _before=null; _session?.CancelGesture(); }
    private void Key(object sender,KeyRoutedEventArgs e)
    {
        if(_session?.Draft is null || _session.Busy) return;
        if(e.Key==VirtualKey.Escape) { Cancel(); _guides.ReleasePointerCaptures(); e.Handled=true; return; }
        var step=Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down)?10:1;
        var p=Placement(); var moved=e.Key switch { VirtualKey.Left=>p with { X=Math.Max(-3840,p.X-step) },VirtualKey.Right=>p with { X=Math.Min(3840,p.X+step) },VirtualKey.Up=>p with { Y=Math.Max(-2160,p.Y-step) },VirtualKey.Down=>p with { Y=Math.Min(2160,p.Y+step) },_=>null };
        if(moved is not null) { _session.SetPlacement(moved); e.Handled=true; }
    }
}
