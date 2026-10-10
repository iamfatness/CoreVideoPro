using CoreVideoPro.MediaCore.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Editor-only guide over a fitted native video surface. No pixels enter the UI.</summary>
public sealed class ScopeRoiControl : UserControl
{
    private readonly Canvas _canvas = new() { Background = new SolidColorBrush(Color.FromArgb(1,0,0,0)) };
    private readonly Rectangle _outline = new() { Stroke = new SolidColorBrush(Color.FromArgb(255,68,193,161)), StrokeThickness = 2, IsHitTestVisible = false };
    private readonly TextBlock _label = new() { Text = "SCOPE ROI", FontSize = 11, IsHitTestVisible = false };
    private readonly List<Rectangle> _handles=[];
    private Point _start;
    private GradeScopeRoi _before = new();
    private bool _drag;
    private int _mode; // 0 draw, 1 move, 2 resize bottom-right
    public GradeScopeRoi Roi { get; private set; } = new();
    public int SourceWidth { get; set; }
    public int SourceHeight { get; set; }
    public event EventHandler<GradeScopeRoi>? RoiChanged;
    public ScopeRoiControl()
    {
        Content = _canvas; _canvas.Children.Add(_outline); _canvas.Children.Add(_label);
        IsTabStop=true;
        for (var i=0;i<8;++i) { var handle=new Rectangle { Width=10,Height=10,Fill=_outline.Stroke,IsHitTestVisible=false };_handles.Add(handle);_canvas.Children.Add(handle); }
        SizeChanged += (_,_) => Draw();
        PointerPressed += Pressed; PointerMoved += Moved;
        PointerReleased += (_,e) => { _drag = false; ReleasePointerCapture(e.Pointer); };
        PointerCaptureLost += (_,_) => _drag = false;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(this,"Scope region: drag to draw, drag inside to move, drag any edge or corner to resize. Arrows move; Shift and arrows resize.");
    }
    public void Update(GradeScopeRoi roi,int width,int height) { Roi=roi; SourceWidth=width; SourceHeight=height; Draw(); }
    private Rect ImageRect {
        get {
            if (ActualWidth<=0 || ActualHeight<=0 || SourceWidth<=0 || SourceHeight<=0) return new();
            var scale = Math.Min(ActualWidth/SourceWidth,ActualHeight/SourceHeight);
            var w=SourceWidth*scale; var h=SourceHeight*scale;
            return new((ActualWidth-w)/2,(ActualHeight-h)/2,w,h);
        }
    }
    private Point Normalize(Point p) { var r=ImageRect; return new(Math.Clamp((p.X-r.X)/r.Width,0,1),Math.Clamp((p.Y-r.Y)/r.Height,0,1)); }
    private void Draw()
    {
        var image=ImageRect; var visible=Roi.Enabled && image.Width>0;
        _outline.Visibility = _label.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        foreach (var handle in _handles) handle.Visibility=_outline.Visibility;
        if (!visible) return;
        _outline.Width=Roi.Width*image.Width; _outline.Height=Roi.Height*image.Height;
        Canvas.SetLeft(_outline,image.X+Roi.X*image.Width); Canvas.SetTop(_outline,image.Y+Roi.Y*image.Height);
        Canvas.SetLeft(_label,image.X+Roi.X*image.Width+5); Canvas.SetTop(_label,image.Y+Roi.Y*image.Height+5);
        var corners=new[] { new Point(0,0),new Point(.5,0),new Point(1,0),new Point(1,.5),new Point(1,1),new Point(.5,1),new Point(0,1),new Point(0,.5) };
        for(var i=0;i<8;++i) {Canvas.SetLeft(_handles[i],image.X+(Roi.X+corners[i].X*Roi.Width)*image.Width-5);Canvas.SetTop(_handles[i],image.Y+(Roi.Y+corners[i].Y*Roi.Height)*image.Height-5);}
    }
    private void Pressed(object sender,PointerRoutedEventArgs e)
    {
        var p=e.GetCurrentPoint(this); var image=ImageRect;
        if (!p.Properties.IsLeftButtonPressed || image.Width<=0 || !image.Contains(p.Position)) return;
        _start=Normalize(p.Position); _before=Roi; _mode=0;
        Focus(FocusState.Pointer);
        if (Roi.Enabled && _start.X>=Roi.X && _start.X<=Roi.X+Roi.Width && _start.Y>=Roi.Y && _start.Y<=Roi.Y+Roi.Height) {
            var left=Math.Abs((_start.X-Roi.X)*image.Width)<14;var right=Math.Abs((_start.X-Roi.X-Roi.Width)*image.Width)<14;
            var top=Math.Abs((_start.Y-Roi.Y)*image.Height)<14;var bottom=Math.Abs((_start.Y-Roi.Y-Roi.Height)*image.Height)<14;
            _mode=left&&top?3:right&&top?4:left&&bottom?5:right&&bottom?2:left?6:top?7:right?8:bottom?9:1;
        }
        _drag=CapturePointer(e.Pointer); e.Handled=true;
    }
    private void Moved(object sender,PointerRoutedEventArgs e)
    {
        if (!_drag) return;
        var p=Normalize(e.GetCurrentPoint(this).Position);
        var minW=1d/Math.Max(1,SourceWidth); var minH=1d/Math.Max(1,SourceHeight);
        GradeScopeRoi next;
        if (_mode==1) next=_before with { X=Math.Clamp(_before.X+p.X-_start.X,0,1-_before.Width), Y=Math.Clamp(_before.Y+p.Y-_start.Y,0,1-_before.Height) };
        else if (_mode>=2) {
            minW=Math.Min(minW,_before.Width);minH=Math.Min(minH,_before.Height);
            var l=_before.X;var t=_before.Y;var r=l+_before.Width;var b=t+_before.Height;
            if (_mode is 3 or 5 or 6) l=Math.Clamp(p.X,0,r-minW);
            if (_mode is 2 or 4 or 8) r=Math.Clamp(p.X,l+minW,1);
            if (_mode is 3 or 4 or 7) t=Math.Clamp(p.Y,0,b-minH);
            if (_mode is 2 or 5 or 9) b=Math.Clamp(p.Y,t+minH,1);
            next=_before with { X=l,Y=t,Width=r-l,Height=b-t };
        }
        else {
            var x=Math.Min(_start.X,p.X); var y=Math.Min(_start.Y,p.Y);
            next=new(true,Math.Min(x,1-minW),Math.Min(y,1-minH),Math.Max(minW,Math.Abs(p.X-_start.X)),Math.Max(minH,Math.Abs(p.Y-_start.Y)),Roi.Revision);
        }
        if (!next.IsValid) return;
        Roi=next; Draw(); RoiChanged?.Invoke(this,next); e.Handled=true;
    }
    protected override void OnKeyDown(KeyRoutedEventArgs e)
    {
        if (!Roi.Enabled || SourceWidth<=0 || SourceHeight<=0) {base.OnKeyDown(e);return;}
        var shift=Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var dx=e.Key==VirtualKey.Left?-1:e.Key==VirtualKey.Right?1:0;var dy=e.Key==VirtualKey.Up?-1:e.Key==VirtualKey.Down?1:0;
        if(dx==0 && dy==0) {base.OnKeyDown(e);return;}
        var next=shift ? Roi with {Width=Math.Clamp(Roi.Width+dx/(double)SourceWidth,Math.Min(1d/SourceWidth,1-Roi.X),1-Roi.X),Height=Math.Clamp(Roi.Height+dy/(double)SourceHeight,Math.Min(1d/SourceHeight,1-Roi.Y),1-Roi.Y)} :
            Roi with {X=Math.Clamp(Roi.X+dx/(double)SourceWidth,0,1-Roi.Width),Y=Math.Clamp(Roi.Y+dy/(double)SourceHeight,0,1-Roi.Height)};
        Roi=next;Draw();RoiChanged?.Invoke(this,next);e.Handled=true;
    }
}
