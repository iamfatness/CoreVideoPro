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
    private readonly Ellipse _circle = new() { Stroke = new SolidColorBrush(Color.FromArgb(255,68,193,161)), StrokeThickness = 2, IsHitTestVisible = false };
    private readonly TextBlock _label = new() { Text = "SCOPE ROI", FontSize = 11, IsHitTestVisible = false };
    private readonly List<Rectangle> _handles=[];
    private Point _start;
    private GradeScopeRoi _before = new();
    private bool _drag;
    private int _mode; // 0 draw, 1 move, 2–9 resize corners/edges
    public GradeScopeRoi Roi { get; private set; } = new();
    public int SourceWidth { get; set; }
    public int SourceHeight { get; set; }
    public event EventHandler<GradeScopeRoi>? RoiChanged;
    public event EventHandler? ToolChanged;
    public string? DrawingShape { get; private set; }
    public void SetDrawingTool(string? shape) { CancelGesture(); DrawingShape=shape; ToolChanged?.Invoke(this,EventArgs.Empty); }
    private void CancelGesture() { if (!_drag) return; _drag=false; Roi=_before; Draw(); RoiChanged?.Invoke(this,Roi); ReleasePointerCaptures(); }
    public ScopeRoiControl()
    {
        Content = _canvas; _canvas.Children.Add(_outline); _canvas.Children.Add(_circle); _canvas.Children.Add(_label);
        IsTabStop=true;
        for (var i=0;i<8;++i) { var handle=new Rectangle { Width=10,Height=10,Fill=_outline.Stroke,IsHitTestVisible=false };_handles.Add(handle);_canvas.Children.Add(handle); }
        SizeChanged += (_,_) => Draw();
        PointerPressed += Pressed; PointerMoved += Moved;
        PointerReleased += (_,e) => {
            if (!_drag) return;
            CompleteDrawingGesture(); ReleasePointerCapture(e.Pointer);
        };
        PointerCaptureLost += (_,_) => CancelGesture();
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(this,"Scope region: drag to draw, drag inside to move, drag any edge or corner to resize. Arrows move; Shift and arrows resize.");
    }
    public void Update(GradeScopeRoi roi,int width,int height) { Roi=roi; SourceWidth=width; SourceHeight=height; Draw(); }
    private Rect ImageRect {
        get {
            if (ActualWidth<=0 || ActualHeight<=0 || SourceWidth<=0 || SourceHeight<=0) return new();
            var r=ScopeRoiGeometry.FitImage(ActualWidth,ActualHeight,SourceWidth,SourceHeight);
            return new(r.X,r.Y,r.Width,r.Height);
        }
    }
    private Point Normalize(Point p) { var r=ImageRect; return new(Math.Clamp((p.X-r.X)/r.Width,0,1),Math.Clamp((p.Y-r.Y)/r.Height,0,1)); }
    private void Draw()
    {
        var image=ImageRect; var visible=Roi.Enabled && image.Width>0;
        _label.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        _outline.Visibility = visible && Roi.Shape=="rectangle" ? Visibility.Visible : Visibility.Collapsed;
        _circle.Visibility = visible && Roi.Shape=="circle" ? Visibility.Visible : Visibility.Collapsed;
        foreach (var handle in _handles) handle.Visibility=_label.Visibility;
        if (!visible) return;
        _outline.Width=Roi.Width*image.Width; _outline.Height=Roi.Height*image.Height;
        Canvas.SetLeft(_outline,image.X+Roi.X*image.Width); Canvas.SetTop(_outline,image.Y+Roi.Y*image.Height);
        _circle.Width=_outline.Width; _circle.Height=_outline.Height;
        Canvas.SetLeft(_circle,image.X+Roi.X*image.Width); Canvas.SetTop(_circle,image.Y+Roi.Y*image.Height);
        Canvas.SetLeft(_label,image.X+Roi.X*image.Width+5); Canvas.SetTop(_label,image.Y+Roi.Y*image.Height+5);
        var corners=new[] { new Point(0,0),new Point(.5,0),new Point(1,0),new Point(1,.5),new Point(1,1),new Point(.5,1),new Point(0,1),new Point(0,.5) };
        for(var i=0;i<8;++i) {Canvas.SetLeft(_handles[i],image.X+(Roi.X+corners[i].X*Roi.Width)*image.Width-5);Canvas.SetTop(_handles[i],image.Y+(Roi.Y+corners[i].Y*Roi.Height)*image.Height-5);}
    }
    private void Pressed(object sender,PointerRoutedEventArgs e)
    {
        var p=e.GetCurrentPoint(this);
        if(!p.Properties.IsLeftButtonPressed || !BeginDrawingGesture(p.Position)) return;
        Focus(FocusState.Pointer);
        _drag=CapturePointer(e.Pointer); e.Handled=true;
    }
    internal bool BeginDrawingGesture(Point position)
    {
        var image=ImageRect;
        if(image.Width<=0 || !image.Contains(position)) return false;
        _start=Normalize(position); _before=Roi; _mode=0;
        if (DrawingShape is null) {
            if(!Roi.Enabled || _start.X<Roi.X-14/image.Width || _start.X>Roi.X+Roi.Width+14/image.Width ||
               _start.Y<Roi.Y-14/image.Height || _start.Y>Roi.Y+Roi.Height+14/image.Height) return false;
            var left=Math.Abs((_start.X-Roi.X)*image.Width)<14;var right=Math.Abs((_start.X-Roi.X-Roi.Width)*image.Width)<14;
            var top=Math.Abs((_start.Y-Roi.Y)*image.Height)<14;var bottom=Math.Abs((_start.Y-Roi.Y-Roi.Height)*image.Height)<14;
            _mode=left&&top?3:right&&top?4:left&&bottom?5:right&&bottom?2:left?6:top?7:right?8:bottom?9:1;
        }
        _drag=true;return true;
    }
    private void Moved(object sender,PointerRoutedEventArgs e)
    {
        if (!_drag) return;
        UpdateDrawingGesture(e.GetCurrentPoint(this).Position);e.Handled=true;
    }
    internal void CompleteDrawingGesture(bool cancel=false)
    {
        if(cancel) CancelGesture();
        _drag=false;DrawingShape=null;ToolChanged?.Invoke(this,EventArgs.Empty);
    }
    internal void UpdateDrawingGesture(Point position)
    {
        if(!_drag || ImageRect.Width<=0) return;
        var p=Normalize(position);
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
            if (_before.Shape=="circle") {
                var diameter = _mode is 6 or 8 ? (r-l)*SourceWidth : _mode is 7 or 9 ? (b-t)*SourceHeight : Math.Min((r-l)*SourceWidth,(b-t)*SourceHeight);
                var movesLeft=_mode is 3 or 5 or 6; var movesTop=_mode is 3 or 4 or 7;
                diameter=Math.Max(1,Math.Min(diameter,Math.Min((movesLeft?r:1-l)*SourceWidth,(movesTop?b:1-t)*SourceHeight)));
                var w=diameter/SourceWidth;var h=diameter/SourceHeight;
                next=next with {X=movesLeft?r-w:l,Y=movesTop?b-h:t,Width=w,Height=h};
            }
        }
        else {
            if (Math.Abs(p.X-_start.X)*ImageRect.Width<3 && Math.Abs(p.Y-_start.Y)*ImageRect.Height<3) return;
            next=ScopeRoiGeometry.Draw(_start.X,_start.Y,p.X,p.Y,SourceWidth,SourceHeight,DrawingShape??"rectangle",Roi.Revision);
        }
        if (!next.IsValid) return;
        Roi=next; Draw(); RoiChanged?.Invoke(this,next);
    }
    protected override void OnKeyDown(KeyRoutedEventArgs e)
    {
        if(e.Key==VirtualKey.Escape) {CompleteDrawingGesture(cancel:true);e.Handled=true;return;}
        if (!Roi.Enabled || SourceWidth<=0 || SourceHeight<=0) {base.OnKeyDown(e);return;}
        var shift=Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);
        var dx=e.Key==VirtualKey.Left?-1:e.Key==VirtualKey.Right?1:0;var dy=e.Key==VirtualKey.Up?-1:e.Key==VirtualKey.Down?1:0;
        if(dx==0 && dy==0) {base.OnKeyDown(e);return;}
        var next=shift ? Roi with {Width=Math.Clamp(Roi.Width+dx/(double)SourceWidth,Math.Min(1d/SourceWidth,1-Roi.X),1-Roi.X),Height=Math.Clamp(Roi.Height+dy/(double)SourceHeight,Math.Min(1d/SourceHeight,1-Roi.Y),1-Roi.Y)} :
            Roi with {X=Math.Clamp(Roi.X+dx/(double)SourceWidth,0,1-Roi.Width),Y=Math.Clamp(Roi.Y+dy/(double)SourceHeight,0,1-Roi.Height)};
        if(shift && Roi.Shape=="circle") next=ScopeRoiGeometry.CircleSize(Roi,Roi.Width*SourceWidth+(dx!=0?dx:dy),SourceWidth,SourceHeight);
        Roi=next;Draw();RoiChanged?.Invoke(this,next);e.Handled=true;
    }
}
