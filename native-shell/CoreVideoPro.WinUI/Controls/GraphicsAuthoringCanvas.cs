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

/// <summary>Editor chrome over native pixels. Layer hit boxes come from native
/// resolved layout; the shell does not reproduce flow, clipping or text layout.</summary>
public sealed class GraphicsAuthoringCanvas : UserControl
{
    private readonly Grid _root=new(),_viewport=new();
    private readonly VideoSurfaceHost _video=new();
    private readonly Canvas _guides=new() {Background=new SolidColorBrush(Microsoft.UI.Colors.Transparent)};
    private readonly Rectangle _safe=new() {Stroke=new SolidColorBrush(Windows.UI.Color.FromArgb(140,180,190,205)),StrokeThickness=1,IsHitTestVisible=false,StrokeDashArray=new DoubleCollection {4,4}};
    private readonly Rectangle _selection=new() {Stroke=new SolidColorBrush(Microsoft.UI.Colors.DeepSkyBlue),StrokeThickness=2,IsHitTestVisible=false};
    private readonly Rectangle _handle=new() {Width=10,Height=10,Fill=new SolidColorBrush(Microsoft.UI.Colors.DeepSkyBlue),IsHitTestVisible=false};
    private GraphicsWorkspaceSession? _session;
    private NativeGraphicsLayout? _lastLayout;private string _layoutContext="";
    private readonly TextBlock _hint=new() {HorizontalAlignment=HorizontalAlignment.Left,VerticalAlignment=VerticalAlignment.Bottom,Margin=new Thickness(8),FontSize=11,IsHitTestVisible=false};
    private readonly Line _vertical=new() {Stroke=new SolidColorBrush(Windows.UI.Color.FromArgb(90,180,190,205)),StrokeThickness=1,IsHitTestVisible=false,StrokeDashArray=new DoubleCollection {3,5}};
    private readonly Line _horizontal=new() {Stroke=new SolidColorBrush(Windows.UI.Color.FromArgb(90,180,190,205)),StrokeThickness=1,IsHitTestVisible=false,StrokeDashArray=new DoubleCollection {3,5}};
    private Point _start;private NativeGraphicsPlacement? _before;
    private NativeGraphicsNodeGeometry? _nodeBefore;private Rect? _optimistic;
    private double _localX,_localY,_localWidth,_localHeight,_width=1050,_height=176;
    private bool _resize;private string _dragLayer="";
    private bool _wholeGraphic;
    public bool WholeGraphic {get=>_wholeGraphic;set {Cancel();_wholeGraphic=value;_optimistic=null;Draw();}}
    private bool Whole=>_wholeGraphic || Down(VirtualKey.Menu);
    public VideoSurfaceState? SurfaceState {get=>_video.SurfaceState;set=>_video.SurfaceState=value;}
    public GraphicsWorkspaceSession? Session {
        get=>_session;
        set {Cancel();if(_session is not null) _session.Changed-=Changed;_session=value;if(_session is not null) _session.Changed+=Changed;Draw();}
    }
    public GraphicsAuthoringCanvas()
    {
        IsTabStop=true;_viewport.HorizontalAlignment=HorizontalAlignment.Center;_viewport.VerticalAlignment=VerticalAlignment.Center;
        _viewport.Children.Add(_video);_viewport.Children.Add(_guides);_viewport.Children.Add(_hint);_root.Children.Add(_viewport);Content=_root;
        _guides.Children.Add(_safe);_guides.Children.Add(_vertical);_guides.Children.Add(_horizontal);_guides.Children.Add(_selection);_guides.Children.Add(_handle);
        SizeChanged+=(_,_)=>Draw();_guides.PointerPressed+=Pressed;_guides.PointerMoved+=Moved;
        _guides.PointerReleased+=Released;_guides.PointerCaptureLost+=(_,_)=>Cancel();KeyDown+=Key;
        AutomationProperties.SetName(this,"Graphics native canvas. Click a layer; drag to move; drag its corner to resize. Alt drags the whole graphic. Arrows nudge; Shift nudges ten pixels; Escape cancels.");
    }
    private void Changed(object? sender,EventArgs e) {if(_before is null && _session?.AuthoringLayout is not null) _optimistic=null;Draw();}
    private NativeGraphicsPlacement Placement()
    {
        if(_session?.Preview is not { } draft) return new();
        return draft.Placement ?? new() {Anchor=draft.Style.Anchor,X=draft.Style.SafeOffsetX*1920*(draft.Style.Anchor.Contains("right",StringComparison.Ordinal)?-1:1),Y=draft.Style.SafeOffsetY*1080*(draft.Style.Anchor.Contains("upper",StringComparison.Ordinal)?1:-1)};
    }
    private static (double X,double Y) Anchor(NativeGraphicsPlacement p)=>(p.Anchor.Contains("right",StringComparison.Ordinal)?1:p.Anchor.Contains("left",StringComparison.Ordinal)?0:.5,p.Anchor.Contains("upper",StringComparison.Ordinal)?0:p.Anchor.Contains("lower",StringComparison.Ordinal)?1:.5);
    private Rect Bounds(NativeGraphicsPlacement p) {var a=Anchor(p);return new(a.X*(1920-_width*p.Scale)+p.X,a.Y*(1080-_height*p.Scale)+p.Y,_width*p.Scale,_height*p.Scale);}
    private Rect CanvasRect(NativeGraphicsNodeGeometry n,NativeGraphicsPlacement p,bool clip=false)
    {
        var root=Bounds(p);return clip?new(root.X+n.ClipX*p.Scale,root.Y+n.ClipY*p.Scale,n.ClipWidth*p.Scale,n.ClipHeight*p.Scale):new(root.X+n.X*p.Scale,root.Y+n.Y*p.Scale,n.Width*p.Scale,n.Height*p.Scale);
    }
    private Rect SelectionBounds()
    {
        if(_optimistic is { } pending) return pending;
        var p=Placement();if(Whole) return Bounds(p);var node=(_session?.AuthoringLayout ?? _lastLayout)?.Nodes.FirstOrDefault(n=>n.Id==_session?.SelectedLayerId);
        return node is null?Bounds(p):CanvasRect(node,p);
    }
    private void Draw()
    {
        var fit=Math.Min(ActualWidth/1920,ActualHeight/1080);if(fit<=0 || !double.IsFinite(fit)) return;
        var context=_session?.State?.CoreEpoch+":"+_session?.SelectedId+":"+_session?.Preview?.TemplateId+":"+_session?.Preview?.TemplateVersion+":"+_session?.CompareOriginal;
        if(context!=_layoutContext) {_layoutContext=context;_lastLayout=null;_optimistic=null;}
        var current=_session?.AuthoringLayout;if(current is not null) _lastLayout=current;
        _hint.Text=_session?.Preview?.Declarative==true && current is null ? "Updating native layer layout…" : Whole ? "Whole graphic — drag to move; corner to scale" : "Click a layer; drag to move; corner to resize";
        _vertical.X1=_vertical.X2=960*fit;_vertical.Y1=0;_vertical.Y2=1080*fit;
        _horizontal.Y1=_horizontal.Y2=540*fit;_horizontal.X1=0;_horizontal.X2=1920*fit;
        _viewport.Width=1920*fit;_viewport.Height=1080*fit;_guides.Clip=new RectangleGeometry {Rect=new(0,0,_viewport.Width,_viewport.Height)};
        _safe.Width=1728*fit;_safe.Height=972*fit;Canvas.SetLeft(_safe,96*fit);Canvas.SetTop(_safe,54*fit);
        var draft=_session?.Preview;_selection.Visibility=_handle.Visibility=draft is null?Visibility.Collapsed:Visibility.Visible;if(draft is null) return;
        if((_session?.AuthoringLayout ?? _lastLayout) is { } layout) {_width=layout.Width;_height=layout.Height;}
        else if(_session?.Template?.Bounds is { } bounds) {_width=bounds.GetProperty("width").GetDouble();_height=bounds.GetProperty("height").GetDouble();}
        else {_width=Math.Min(draft.Style.Width,1-draft.Style.SafeOffsetX)*1920;_height=Math.Min(.4,(draft.Style.NameSize+draft.Style.TitleSize+draft.Style.Padding*3)/1080)*1080;}
        var rect=SelectionBounds();_selection.Width=Math.Max(0,rect.Width*fit);_selection.Height=Math.Max(0,rect.Height*fit);
        Canvas.SetLeft(_selection,rect.X*fit);Canvas.SetTop(_selection,rect.Y*fit);Canvas.SetLeft(_handle,rect.Right*fit-5);Canvas.SetTop(_handle,rect.Bottom*fit-5);
    }
    private static bool Down(VirtualKey key)=>Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
    private double LocalNumber(string id,string property,double fallback)
    {
        if(_session?.TemplateDocument is not { } document) return fallback;var layer=GraphicsTemplateEditor.Layer(document,id);
        if(!layer.TryGetProperty(property,out var value)) return fallback;
        if(value.ValueKind==System.Text.Json.JsonValueKind.Object && value.TryGetProperty("field",out var field) && _session.Draft?.Fields.TryGetProperty(field.GetString()!,out var bound)==true) value=bound;
        return value.ValueKind==System.Text.Json.JsonValueKind.Number?value.GetDouble():fallback;
    }
    private void Pressed(object sender,PointerRoutedEventArgs e)
    {
        if(_session?.Draft is null || _session.Busy || _session.CompareOriginal || _session.PreviewSample!="current" || !e.GetCurrentPoint(_guides).Properties.IsLeftButtonPressed) return;
        var fit=_viewport.Width/1920;if(fit<=0 || !double.IsFinite(fit)) return;
        Focus(FocusState.Pointer);var point=e.GetCurrentPoint(_guides).Position;var reference=new Point(point.X/fit,point.Y/fit);var p=Placement();
        var selection=Whole?Bounds(p):SelectionBounds();_resize=Math.Abs(reference.X-selection.Right)*fit<14 && Math.Abs(reference.Y-selection.Bottom)*fit<14;
        var layout=_session.AuthoringLayout;
        NativeGraphicsNodeGeometry? node=null;
        if(!Whole && layout is not null) node=_resize?layout.Nodes.FirstOrDefault(n=>n.Id==_session.SelectedLayerId):layout.Nodes.Reverse().FirstOrDefault(n=>n.Visible && CanvasRect(n,p,true).Contains(reference));
        if(node is not null) {
            if(_session.TemplateDocument is not { } document || GraphicsTemplateEditor.Layers(document).FirstOrDefault(l=>l.Id==node.Id)?.Locked==true) return;
            _session.SelectLayer(node.Id);_dragLayer=node.Id;_nodeBefore=node;
            _localX=LocalNumber(node.Id,"x",0);_localY=LocalNumber(node.Id,"y",0);_localWidth=LocalNumber(node.Id,"width",node.Width);_localHeight=LocalNumber(node.Id,"height",node.Height);
        } else {
            if(_session.Draft.Declarative && !Whole) return;
            if(!_resize && !Bounds(p).Contains(reference)) return;_dragLayer="";_nodeBefore=null;
        }
        _start=reference;_before=p;_session.BeginGesture();if(!_guides.CapturePointer(e.Pointer)) Cancel();e.Handled=true;
    }
    private void Moved(object sender,PointerRoutedEventArgs e)
    {
        if(_before is not { } before || _session is null) return;var fit=_viewport.Width/1920;if(fit<=0) return;
        var point=e.GetCurrentPoint(_guides).Position;var dx=point.X/fit-_start.X;var dy=point.Y/fit-_start.Y;
        if(_nodeBefore is { } node) {
            if(!_resize) {
                var x=Math.Clamp(_localX+dx/before.Scale,0,8192);var y=Math.Clamp(_localY+dy/before.Scale,0,8192);var rect=CanvasRect(node,before);
                _optimistic=new(rect.X+(x-_localX)*before.Scale,rect.Y+(y-_localY)*before.Scale,rect.Width,rect.Height);
                _session.SetLayerNumbers(_dragLayer,new Dictionary<string,double> {{"x",x},{"y",y}});
            } else {
                var w=Math.Clamp(_localWidth+dx/before.Scale,1,8192);var h=Math.Clamp(_localHeight+dy/before.Scale,1,8192);
                if(node.Type=="image" || Down(VirtualKey.Shift)) {var desired=Math.Abs(dx/_localWidth)>=Math.Abs(dy/_localHeight)?w/_localWidth:h/_localHeight;var scale=Math.Clamp(desired,1/Math.Min(_localWidth,_localHeight),8192/Math.Max(_localWidth,_localHeight));w=_localWidth*scale;h=_localHeight*scale;}
                var rect=CanvasRect(node,before);_optimistic=new(rect.X,rect.Y,w*before.Scale,h*before.Scale);
                _session.SetLayerNumbers(_dragLayer,new Dictionary<string,double> {{"width",w},{"height",h}});
            }
        } else if(!_resize) _session.SetPlacement(before with {X=Math.Clamp(before.X+dx,-3840,3840),Y=Math.Clamp(before.Y+dy,-2160,2160)});
        else {
            var delta=Math.Abs(dx/Math.Max(_width,1))>=Math.Abs(dy/Math.Max(_height,1))?dx/Math.Max(_width,1):dy/Math.Max(_height,1);var scale=Math.Clamp(before.Scale+delta,.1,4);var a=Anchor(before);
            _session.SetPlacement(before with {Scale=scale,X=Math.Clamp(before.X+a.X*_width*(scale-before.Scale),-3840,3840),Y=Math.Clamp(before.Y+a.Y*_height*(scale-before.Scale),-2160,2160)});
        }
        Draw();e.Handled=true;
    }
    private void Released(object sender,PointerRoutedEventArgs e) {if(_before is null) return;_before=null;_nodeBefore=null;_session?.EndGesture();_guides.ReleasePointerCapture(e.Pointer);e.Handled=true;}
    private void Cancel() {if(_before is null) return;_before=null;_nodeBefore=null;_optimistic=null;_session?.CancelGesture();Draw();}
    private void Key(object sender,KeyRoutedEventArgs e)
    {
        if(_session?.Draft is null || _session.Busy || _session.CompareOriginal || _session.PreviewSample!="current") return;
        if(e.Key==VirtualKey.Escape) {Cancel();_guides.ReleasePointerCaptures();e.Handled=true;return;}
        var dx=e.Key==VirtualKey.Left?-1:e.Key==VirtualKey.Right?1:0;var dy=e.Key==VirtualKey.Up?-1:e.Key==VirtualKey.Down?1:0;if(dx==0 && dy==0) return;
        var step=Down(VirtualKey.Shift)?10:1;var p=Placement();
        if(_session.Draft.Declarative && !Whole) {
            var id=_session.SelectedLayerId;if(_session.TemplateDocument is not { } document || !GraphicsTemplateEditor.Layers(document).Any(n=>n.Id==id)) return;
            var rect=SelectionBounds();var x=LocalNumber(id,"x",0);var y=LocalNumber(id,"y",0);
            _session.SetLayerNumbers(id,new Dictionary<string,double> {{"x",Math.Clamp(x+dx*step/p.Scale,0,8192)},{"y",Math.Clamp(y+dy*step/p.Scale,0,8192)}});
            if(_session.AuthoringLayout is null) _optimistic=new(rect.X+(LocalNumber(id,"x",0)-x)*p.Scale,rect.Y+(LocalNumber(id,"y",0)-y)*p.Scale,rect.Width,rect.Height);
            Draw();
        } else _session.SetPlacement(p with {X=Math.Clamp(p.X+dx*step,-3840,3840),Y=Math.Clamp(p.Y+dy*step,-2160,2160)});
        e.Handled=true;
    }
}
