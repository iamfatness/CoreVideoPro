using System.Globalization;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsDesignerControl
{
    private readonly Button _playPreview = StudioBrushes.GhostButton("Play preview");
    private readonly Button _motionExpand = StudioBrushes.GhostButton("Show motion");
    private readonly Button _motionIn = StudioBrushes.GhostButton("In"), _motionOut = StudioBrushes.GhostButton("Out");
    private readonly Button _playIn = StudioBrushes.GhostButton("Play In"), _playOut = StudioBrushes.GhostButton("Play Out");
    private readonly ToggleButton _motionLoop = new() { Content = "Loop", FontSize = 13 };
    private readonly CheckBox _motionMirror = new() { Content = "Out mirrors In", FontSize = 13, MinWidth = 0 };
    private readonly StackPanel _motionActions = new() { Orientation = Orientation.Horizontal, Spacing = 5 };
    private readonly StackPanel _motionBody = new() { Spacing = 1 };
    private readonly StackPanel _motionRows = new();
    private readonly TextBlock _motionHint = new() { FontSize = 12, Foreground = StudioBrushes.Muted, TextWrapping = TextWrapping.Wrap, MaxLines = 2, VerticalAlignment = VerticalAlignment.Center };
    private readonly Canvas _motionScale = new() { Height = 17 };
    private readonly List<(GraphicsMotionBar Bar, Button Button)> _motionButtons = [];
    private string _motionPhase = "in", _motionSignature = "";
    private bool _motionWide, _motionExpanded;
    private bool? _motionExpansionOverride;
    private double _motionRange = 1000;
    private MotionDrag? _motionDrag;
    private bool _motionDragMoved;

    private sealed record MotionDrag(GraphicsMotionBar Bar, GraphicsLayerMotion Motion, string Phase,
        string Handle, double StartX, double Range, Canvas Track, Button Button, uint PointerId);

    private Border BuildMotionStrip()
    {
        var stack = new StackPanel { Spacing = 3 };
        var header = new Grid { ColumnSpacing = 6 };
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(Heading("MOTION"));
        Grid.SetColumn(_motionActions, 1); header.Children.Add(_motionActions);
        foreach (var button in new[] { _motionIn, _motionOut, _playIn, _playOut })
        {
            button.FontSize = 13; button.Padding = new Thickness(6, 3, 6, 3);
            _motionActions.Children.Add(button);
        }
        _motionActions.Children.Add(_motionLoop);
        _motionActions.Children.Add(_motionMirror);
        _motionExpand.FontSize = 13;
        _motionExpand.Padding = new Thickness(7, 3, 7, 3);
        Grid.SetColumn(_motionHint, 2); header.Children.Add(_motionHint);
        Grid.SetColumn(_motionExpand, 3); header.Children.Add(_motionExpand);
        AutomationProperties.SetName(_motionIn, "Show the way in");
        AutomationProperties.SetName(_motionOut, "Show the way out");
        AutomationProperties.SetName(_playIn, "Play In");
        AutomationProperties.SetName(_playOut, "Play Out");
        AutomationProperties.SetName(_motionLoop, "Loop preview");
        AutomationProperties.SetName(_motionMirror, "Out mirrors In");
        _motionExpand.Click += (_, _) => { _motionExpansionOverride = !_motionExpanded; ReflowMotion(_motionWide); };
        _motionIn.Click += (_, _) => SetMotionPhase("in");
        _motionOut.Click += (_, _) => SetMotionPhase("out");
        _playIn.Click += (_, _) => PlayMotion("in");
        _playOut.Click += (_, _) => PlayMotion("out");
        _motionLoop.Click += (_, _) =>
        {
            if (_motionLoop.IsChecked == true) PlayMotion("loop");
            else _session?.StopPreviewPlay();
        };
        _motionMirror.Click += (_, _) =>
        {
            CommitPendingText();
            if (!Apply(scratch => GraphicsTemplateEditor.SetOutMirrorsIn(scratch, _motionMirror.IsChecked == true))) _motionSignature = "";
            RefreshMotionStrip();
        };
        stack.Children.Add(header);
        var scaleRow = MotionRow();
        Grid.SetColumn(_motionScale, 1); scaleRow.Children.Add(_motionScale);
        _motionScale.SizeChanged += (_, _) => DrawMotionScale();
        _motionBody.Children.Add(scaleRow);
        _motionBody.Children.Add(new ScrollViewer { Content = _motionRows, MaxHeight = 132,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled });
        stack.Children.Add(_motionBody);
        ReflowMotion(false);
        return new Border { Child = stack, Background = StudioBrushes.Panel, BorderBrush = StudioBrushes.Border,
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(8, 2, 8, 2) };
    }

    private static Grid MotionRow()
    {
        var row = new Grid { ColumnSpacing = 6 };
        row.ColumnDefinitions.Add(new() { Width = new GridLength(140) });
        row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        return row;
    }

    private void ReflowMotion(bool wide)
    {
        _motionWide = wide;
        _motionExpanded = _motionExpansionOverride ?? wide;
        _motionActions.Visibility = _motionBody.Visibility = _motionHint.Visibility = _motionExpanded ? Visibility.Visible : Visibility.Collapsed;
        _motionExpand.Content = _motionExpanded ? "⌃" : "Show motion";
        AutomationProperties.SetName(_motionExpand, _motionExpanded ? "Hide motion" : "Show motion");
        if (_motionExpanded) RefreshMotionStrip();
    }

    private void SetMotionPhase(string phase)
    {
        CommitPendingText();
        _motionPhase = phase;
        RefreshAll();
    }

    private void PlayMotion(string mode)
    {
        if (!CanEdit || _session is not { } session) return;
        if (_pending.Owner.Length > 0 && !_pending.Flush()) return;
        session.PlayPreview(mode);
        Say(mode switch
        {
            "in" => "Playing the way in on the private preview.",
            "out" => "Playing the way out on the private preview.",
            "loop" => "Looping motion on the private preview.",
            _ => "Playing the way in and out on the private preview."
        });
    }

    private void RefreshMotionStrip()
    {
        RefreshMotionPlayback();
        if (!_motionExpanded || _motionDrag is not null || _session is not { } session) return;
        try
        {
            var document = session.TemplateDocument;
            var signature = $"{document?.Hash}:{_motionPhase}:{CanEdit}";
            if (signature != _motionSignature)
            {
                _motionSignature = signature;
                _motionRows.Children.Clear();
                _motionButtons.Clear();
                _motionMirror.IsEnabled = CanEdit;
                if (document is null) return;
                var mirror = GraphicsTemplateEditor.OutMirrorsIn(document);
                _motionMirror.IsChecked = mirror;
                var inTotal = GraphicsMotionModel.Total(document, "in");
                var outTotal = GraphicsMotionModel.Total(document, "out");
                _motionIn.Content = $"In · {inTotal / 1000:0.0#} s";
                _motionOut.Content = $"Out · {outTotal / 1000:0.0#} s";
                var total = _motionPhase == "in" ? inTotal : outTotal;
                _motionRange = Math.Clamp(Math.Ceiling((total + 200) / 200) * 200, 1000, 3000);
                var readOnly = _motionPhase == "out" && mirror;
                _motionHint.Text = readOnly ? "Turn off Out mirrors In to edit the way out." : "Each bar is one layer's move. Drag to retime.";
                foreach (var bar in GraphicsMotionModel.Bars(document, _motionPhase))
                    AddMotionBar(bar, CanEdit && !readOnly);
                DrawMotionScale();
            }
            _motionIn.BorderBrush = _motionPhase == "in" ? StudioBrushes.Accent : StudioBrushes.Border;
            _motionOut.BorderBrush = _motionPhase == "out" ? StudioBrushes.Accent : StudioBrushes.Border;
            foreach (var (bar, button) in _motionButtons)
                button.BorderBrush = session.SelectedLayerId == bar.LayerId ? StudioBrushes.Accent : StudioBrushes.Border;
        }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or System.Text.Json.JsonException)
        {
            Say(ex.Message);
        }
    }

    private void RefreshMotionPlayback()
    {
        _motionLoop.IsChecked = _session?.PreviewPlay?.Mode == "loop";
        _playIn.IsEnabled = _playOut.IsEnabled = _motionLoop.IsEnabled = CanEdit;
    }

    private void DrawMotionScale()
    {
        _motionScale.Children.Clear();
        var step = _motionRange <= 1400 ? 200 : 500;
        for (var ms = 0; ms <= _motionRange; ms += step)
        {
            var tick = new TextBlock { Text = ms == 0 ? "0 s" : (ms / 1000d).ToString("0.0", CultureInfo.InvariantCulture),
                FontSize = 11, Foreground = StudioBrushes.Muted };
            Canvas.SetLeft(tick, Math.Max(0, Math.Min(_motionScale.ActualWidth - 21, _motionScale.ActualWidth * ms / _motionRange)));
            _motionScale.Children.Add(tick);
        }
    }

    private static Brush MotionBrush(GraphicsMotionBar bar)
    {
        if (bar.Follows) return StudioBrushes.Border;
        var color = bar.Type switch
        {
            "wipe" => 0x4A7CC9, "grow" => 0xB5463F, "rise" => 0x2E8A73, "slide" => 0x8A6E2E,
            "pop" => 0x6B5BAA, _ => 0x2E6E8A
        };
        return new SolidColorBrush(Windows.UI.Color.FromArgb(255, (byte)(color >> 16), (byte)(color >> 8), (byte)color));
    }

    private void AddMotionBar(GraphicsMotionBar bar, bool editable)
    {
        var row = MotionRow();
        row.Height = 22;
        row.Children.Add(new TextBlock { Text = bar.Label, Margin = new Thickness(8 * bar.Depth, 0, 0, 0),
            TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center,
            FontSize = 13, Foreground = StudioBrushes.Text });
        var track = new Canvas { Background = StudioBrushes.Field, Height = 21 };
        Grid.SetColumn(track, 1); row.Children.Add(track);
        var button = StudioBrushes.GhostButton("");
        button.MinWidth = 0; button.MinHeight = 0; button.Height = 20; button.Padding = new Thickness(4, 0, 4, 0);
        button.HorizontalContentAlignment = HorizontalAlignment.Left;
        button.Background = MotionBrush(bar);
        button.Content = new TextBlock { Text = bar.Caption, TextTrimming = TextTrimming.CharacterEllipsis,
            FontSize = 11, Foreground = bar.Follows ? StudioBrushes.Muted : StudioBrushes.Text };
        button.IsEnabled = editable;
        ToolTipService.SetToolTip(button, bar.Tooltip);
        button.Click += (_, _) => SelectLayer(bar.LayerId);
        button.KeyDown += (_, e) =>
        {
            if (e.Key is not (VirtualKey.Left or VirtualKey.Right) || !editable) return;
            e.Handled = true;
            var direction = e.Key == VirtualKey.Left ? -50 : 50;
            var handle = KeyDownNow(VirtualKey.Shift) ? "end" : "body";
            ApplyMotionDrag(bar, _motionPhase, handle, direction, fine: false);
        };
        button.AddHandler(PointerPressedEvent, new PointerEventHandler((_, e) => BeginMotionDrag(bar, track, button, e)), true);
        button.AddHandler(PointerMovedEvent, new PointerEventHandler((_, e) => MoveMotionDrag(e)), true);
        button.AddHandler(PointerReleasedEvent, new PointerEventHandler((_, e) =>
        {
            if (_motionDrag?.PointerId != e.Pointer.PointerId) return;
            EndMotionDrag();
            button.ReleasePointerCapture(e.Pointer);
        }), true);
        button.PointerCaptureLost += (_, e) => { if (_motionDrag?.PointerId == e.Pointer.PointerId) EndMotionDrag(); };
        track.Children.Add(button);
        track.SizeChanged += (_, _) => SizeMotionBar(track, button, bar, bar.StartMs, bar.DurationMs, _motionRange);
        SizeMotionBar(track, button, bar, bar.StartMs, bar.DurationMs, _motionRange);
        _motionButtons.Add((bar, button));
        _motionRows.Children.Add(row);
    }

    private static bool KeyDownNow(VirtualKey key) =>
        Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    private static void SizeMotionBar(Canvas track, Button button, GraphicsMotionBar bar, double start, double duration, double range)
    {
        var scale = track.ActualWidth / range;
        var left = Math.Clamp(start * scale, 0, Math.Max(0, track.ActualWidth - 12));
        Canvas.SetLeft(button, left);
        button.Width = Math.Max(0, Math.Min(track.ActualWidth - left, Math.Max(12, duration * scale)));
        AutomationProperties.SetName(button, FormattableString.Invariant($"{bar.Label} motion, starts {start / 1000:0.00} s, takes {duration / 1000:0.00} s"));
    }

    private GraphicsLayerMotion MotionForBar(NativeGraphicsScratch scratch, GraphicsMotionBar bar, string phase)
    {
        var own = GraphicsTemplateEditor.LayerMotion(scratch, bar.LayerId, phase);
        if (own is not null) return own;
        var graphic = GraphicsTemplateEditor.GraphicMotion(scratch);
        var transition = phase == "in" ? graphic.In : graphic.Out;
        var motion = GraphicsMotionModel.DefaultFor(bar.Type);
        return motion with { DurationMs = bar.Type == "cut" ? 0 : Math.Max(50, bar.DurationMs),
            Easing = transition.Easing, Direction = bar.Type == "slide" ? transition.Direction : motion.Direction };
    }

    private void ApplyMotionDrag(GraphicsMotionBar bar, string phase, string handle, double delta, bool fine)
    {
        Apply(scratch =>
        {
            var motion = MotionForBar(scratch, bar, phase);
            var (start, duration) = GraphicsMotionModel.Drag(motion.StartMs, motion.DurationMs, handle, delta, fine);
            return GraphicsTemplateEditor.SetLayerMotion(scratch, bar.LayerId, phase, motion with { StartMs = start, DurationMs = duration });
        });
    }

    private void BeginMotionDrag(GraphicsMotionBar bar, Canvas track, Button button, PointerRoutedEventArgs e)
    {
        if (!button.IsEnabled || _motionDrag is not null || !e.GetCurrentPoint(button).Properties.IsLeftButtonPressed ||
            _session is not { } session || session.TemplateDocument is not { } scratch || track.ActualWidth <= 0) return;
        try
        {
            var motion = MotionForBar(scratch, bar, _motionPhase);
            var localX = e.GetCurrentPoint(button).Position.X;
            var edge = Math.Min(8, button.ActualWidth / 3);
            var handle = motion.DurationMs == 0 ? "body" :
                localX <= edge ? "start" : localX >= button.ActualWidth - edge ? "end" : "body";
            if (!button.CapturePointer(e.Pointer)) return;
            _motionDrag = new(bar, motion, _motionPhase, handle, e.GetCurrentPoint(track).Position.X,
                _motionRange, track, button, e.Pointer.PointerId);
            _motionDragMoved = false;
            SelectLayer(bar.LayerId);
            session.BeginGesture();
            e.Handled = true;
        }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or System.Text.Json.JsonException)
        {
            Say(ex.Message);
        }
    }

    private void MoveMotionDrag(PointerRoutedEventArgs e)
    {
        if (_motionDrag is not { } drag || drag.PointerId != e.Pointer.PointerId || drag.Track.ActualWidth <= 0) return;
        var pixels = e.GetCurrentPoint(drag.Track).Position.X - drag.StartX;
        if (Math.Abs(pixels) < 2 && !_motionDragMoved) return;
        _motionDragMoved = true;
        var (start, duration) = GraphicsMotionModel.Drag(drag.Motion.StartMs, drag.Motion.DurationMs, drag.Handle,
            pixels / drag.Track.ActualWidth * drag.Range, KeyDownNow(VirtualKey.Control));
        if (Apply(scratch => GraphicsTemplateEditor.SetLayerMotion(scratch, drag.Bar.LayerId, drag.Phase,
                drag.Motion with { StartMs = start, DurationMs = duration })))
            SizeMotionBar(drag.Track, drag.Button, drag.Bar, start, duration, drag.Range);
        e.Handled = true;
    }

    private void EndMotionDrag()
    {
        _motionDrag = null;
        _session?.EndGesture();
        DispatcherQueue.TryEnqueue(RefreshAll);
    }
}
