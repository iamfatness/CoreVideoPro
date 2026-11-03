using System.Globalization;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Windows.System;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsDesignerControl
{
    private readonly Button _motionTab = StudioBrushes.GhostButton("Motion");
    private readonly StackPanel _motionPanel = new() { Spacing = 8, Padding = new Thickness(0, 0, 8, 8) };
    private readonly ScrollViewer _motionScroll = new() { VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, Visibility = Visibility.Collapsed };
    private string _motionPanelSignature = "";

    private void RefreshMotionPanel(IReadOnlyList<GraphicsDesignerRow> rows)
    {
        if (_motionScroll.Visibility != Visibility.Visible || _session is not { } session || _motionDrag is not null) return;
        try
        {
            var row = rows.FirstOrDefault(layer => layer.Id == session.SelectedLayerId);
            var document = session.TemplateDocument;
            var signature = $"{document?.Hash}:{row?.Id}:{_motionPhase}:{CanEdit}";
            _inspectorTitle.Text = row is null ? "NO LAYER SELECTED" :
                $"{row.Label.ToUpperInvariant()} · {(_motionPhase == "in" ? "HOW IT COMES IN" : "HOW IT LEAVES")}";
            if (_motionPanelSignature == signature) return;
            _motionPanelSignature = signature;
            _motionPanel.Children.Clear();
            if (row is null || document is null)
            {
                _motionPanel.Children.Add(Note("Choose a layer to set its motion."));
                return;
            }
            BuildMotionPanel(row.Id, document);
        }
        catch (Exception ex) when (ex is InvalidDataException or InvalidOperationException or System.Text.Json.JsonException)
        {
            Say(ex.Message);
        }
    }

    private void BuildMotionPanel(string id, NativeGraphicsScratch document)
    {
        var phase = _motionPhase;
        var readOnly = phase == "out" && GraphicsTemplateEditor.OutMirrorsIn(document);
        var canEdit = CanEdit && !readOnly;
        var own = GraphicsTemplateEditor.LayerMotion(document, id, phase);
        var follows = new CheckBox { Content = "Moves with the graphic", IsChecked = own is null, IsEnabled = canEdit,
            FontSize = 14, MinWidth = 0 };
        AutomationProperties.SetName(follows, "Moves with the graphic");
        follows.Click += (_, _) =>
        {
            CommitPendingText();
            var applied = Apply(scratch =>
            {
                if (follows.IsChecked == true) return GraphicsTemplateEditor.SetLayerMotion(scratch, id, phase, null);
                var bar = GraphicsMotionModel.Bars(scratch, phase).Single(item => item.LayerId == id);
                return GraphicsTemplateEditor.SetLayerMotion(scratch, id, phase, MotionForBar(scratch, bar, phase));
            });
            if (!applied) _motionPanelSignature = "";
            RefreshAll();
        };
        _motionPanel.Children.Add(follows);
        if (readOnly) _motionPanel.Children.Add(Note("Turn off Out mirrors In to edit the way out."));

        var moves = new Grid { RowSpacing = 4, ColumnSpacing = 4 };
        for (var i = 0; i < 4; i++) moves.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        for (var i = 0; i < 2; i++) moves.RowDefinitions.Add(new() { Height = GridLength.Auto });
        for (var i = 0; i < GraphicsMotionModel.Moves.Count; i++)
        {
            var (type, label) = GraphicsMotionModel.Moves[i];
            var button = StudioBrushes.GhostButton(label);
            button.Content = new TextBlock { Text = label, TextWrapping = TextWrapping.Wrap, TextAlignment = TextAlignment.Center };
            button.FontSize = 12; button.MinWidth = 0; button.MinHeight = 31; button.Padding = new Thickness(2, 2, 2, 2);
            button.HorizontalAlignment = HorizontalAlignment.Stretch;
            // Choosing a move is also a shortcut for giving this layer its own motion.
            button.IsEnabled = canEdit;
            if (own?.Type == type) { button.BorderBrush = StudioBrushes.Accent; button.Foreground = StudioBrushes.Accent; }
            AutomationProperties.SetName(button, label);
            button.Click += (_, _) =>
            {
                CommitPendingText();
                Apply(scratch =>
                {
                    var start = GraphicsTemplateEditor.LayerMotion(scratch, id, phase)?.StartMs ?? 0;
                    var move = GraphicsMotionModel.DefaultFor(type);
                    return GraphicsTemplateEditor.SetLayerMotion(scratch, id, phase,
                        move with { StartMs = Math.Min(start, 3000 - move.DurationMs) });
                });
            };
            Grid.SetColumn(button, i % 4); Grid.SetRow(button, i / 4); moves.Children.Add(button);
        }
        _motionPanel.Children.Add(moves);
        var timing = new Grid { ColumnSpacing = 6 };
        timing.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        timing.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        var basis = own ?? GraphicsMotionModel.DefaultFor("cut");
        var startBox = MotionTime(id, phase, "Starts at", basis.StartMs, duration: false, canEdit && own is not null);
        var durationBox = MotionTime(id, phase, "Takes", basis.DurationMs, duration: true, canEdit && own is not null && basis.Type != "cut");
        timing.Children.Add(startBox);
        Grid.SetColumn(durationBox, 1); timing.Children.Add(durationBox);
        _motionPanel.Children.Add(timing);
        _motionPanel.Children.Add(MotionChoice(id, phase, "Feel", GraphicsMotionModel.Feels, basis.Easing,
            canEdit && own is not null, (move, choice) => move with { Easing = choice }));
        if (GraphicsMotionModel.Directions(basis.Type) is { Count: > 0 } directions)
            _motionPanel.Children.Add(MotionChoice(id, phase, basis.Type == "grow" ? "Grows" : "From", directions,
                basis.Direction, canEdit, (move, choice) => move with { Direction = choice }));
        if (GraphicsMotionModel.Distances(basis.Type) is { Count: > 0 } distances)
            _motionPanel.Children.Add(MotionChoice(id, phase, basis.Type == "rise" ? "Rises by" : "Travels", distances,
                basis.FromEdge ? "edge" : basis.Distance?.ToString(CultureInfo.InvariantCulture), canEdit,
                (move, choice) => move with { FromEdge = choice == "edge", Distance = choice == "edge" ? null : double.Parse(choice, CultureInfo.InvariantCulture) }));
        _motionPanel.Children.Add(Heading("WHOLE-GRAPHIC PRESETS"));
        foreach (var (preset, label) in GraphicsMotionModel.Presets)
        {
            var button = StudioBrushes.GhostButton(label);
            button.FontSize = 13; button.Padding = new Thickness(6, 3, 6, 3);
            button.HorizontalAlignment = HorizontalAlignment.Stretch;
            button.IsEnabled = canEdit;
            AutomationProperties.SetName(button, label);
            button.Click += (_, _) => { CommitPendingText(); Apply(scratch => GraphicsTemplateEditor.ApplyMotionPreset(scratch, preset)); };
            _motionPanel.Children.Add(button);
        }
        _motionPanel.Children.Add(Note("A preset fills every bar; adjust any one afterwards."));
    }

    private TextBox MotionTime(string id, string phase, string label, double milliseconds, bool duration, bool enabled)
    {
        var initial = FormattableString.Invariant($"{milliseconds / 1000:0.00#} s");
        var box = new TextBox { Header = label, Text = initial,
            IsEnabled = enabled, MinWidth = 0, FontSize = 14 };
        AutomationProperties.SetName(box, label);
        bool Commit()
        {
            try
            {
                var value = GraphicsMotionModel.ParseSeconds(box.Text) * 1000;
                return Apply(scratch =>
                {
                    var own = GraphicsTemplateEditor.LayerMotion(scratch, id, phase)
                        ?? throw new InvalidOperationException("Give this layer its own motion first.");
                    return GraphicsTemplateEditor.SetLayerMotion(scratch, id, phase,
                        duration ? own with { DurationMs = value } : own with { StartMs = value });
                });
            }
            catch (InvalidDataException ex) { Say(ex.Message); return false; }
        }
        box.TextChanging += (_, _) => { if (!_loading && box.Text != initial) _pending.Track(id, Commit); };
        box.KeyDown += (_, e) => { if (e.Key == VirtualKey.Enter) { e.Handled = true; CommitPendingText(); } };
        box.LostFocus += (_, _) => DispatcherQueue.TryEnqueue(CommitPendingText);
        return box;
    }

    private ComboBox MotionChoice(string id, string phase, string label, IReadOnlyList<(string Id, string Label)> choices,
        string? selected, bool enabled, Func<GraphicsLayerMotion, string, GraphicsLayerMotion> change)
    {
        var combo = new ComboBox { Header = label, HorizontalAlignment = HorizontalAlignment.Stretch, IsEnabled = enabled };
        foreach (var choice in choices) combo.Items.Add(new ComboBoxItem { Content = choice.Label, Tag = choice.Id });
        combo.SelectedItem = combo.Items.OfType<ComboBoxItem>().FirstOrDefault(item => item.Tag as string == selected);
        AutomationProperties.SetName(combo, label);
        combo.SelectionChanged += (_, _) =>
        {
            if (_loading || combo.SelectedItem is not ComboBoxItem { Tag: string choice }) return;
            DispatcherQueue.TryEnqueue(() => Apply(scratch =>
            {
                var own = GraphicsTemplateEditor.LayerMotion(scratch, id, phase)
                    ?? throw new InvalidOperationException("Give this layer its own motion first.");
                return GraphicsTemplateEditor.SetLayerMotion(scratch, id, phase, change(own, choice));
            }));
        };
        return combo;
    }
}
