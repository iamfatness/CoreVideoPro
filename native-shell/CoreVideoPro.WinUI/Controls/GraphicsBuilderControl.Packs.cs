using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Graphics styles slice 11 (spec section 5): the Builder reshaped around style packs. The style strip
/// lists installed packs; their cards are thumbnails the core renders in the current show palette; a pack look
/// exposes the same knobs (colours by role, position + safe area, size, motion speed, optional elements); the
/// private preview plays in, out or loops; "Add speaker" adds a line to the names list; Save to show / Save &amp; Cue.
/// Pure decisions live in <see cref="GraphicsBuilder"/> (GraphicsBuilder.Packs.cs).</summary>
public sealed partial class GraphicsBuilderControl
{
    private const string PackMotionId = "pack";
    private static readonly NativeGraphicsAnimations PackFade = new() {
        In = new() { Type = "fade", DurationMs = 300, Easing = "ease-out" }, Out = new() { Type = "fade", DurationMs = 200, Easing = "ease-in" } };

    private readonly VariableSizedWrapGrid _styles = new() { Orientation = Orientation.Horizontal, ItemWidth = 84, ItemHeight = 34, Margin = new Thickness(0, 0, 0, 2) };
    private readonly StackPanel _elements = new() { Spacing = 4 };
    private readonly CheckBox _secondLine = new() { Content = "Second line", MinWidth = 0 }, _tagLine = new() { Content = "Tag", MinWidth = 0 };
    private readonly StackPanel _packPlacement = new() { Spacing = 6, Margin = new Thickness(0, 6, 0, 0) };
    private readonly ToggleButton[] _positionButtons = GraphicsBuilder.PositionChoices.Select(c => Segment(c.Label, c.Id)).ToArray();
    private readonly ToggleButton[] _sizeButtons = GraphicsBuilder.SizeChoices.Select(c => Segment(c.Label, c.Id)).ToArray();
    private readonly CheckBox _safeArea = new() { Content = "Safe area 90 %", IsChecked = true, MinWidth = 0 };
    private readonly Button _playIn = StudioBrushes.ChipButton("▶ Play in"), _playOutButton = StudioBrushes.ChipButton("▶ Play out");
    private readonly ToggleButton _loop = new() { Content = "⟲ Loop", FontSize = 11, Padding = new Thickness(8, 2, 8, 2), MinHeight = 22, MinWidth = 0, CornerRadius = new CornerRadius(10) };
    private readonly Button _saveCue = StudioBrushes.GhostButton("Save & Cue");
    private readonly Button _addSpeaker = StudioBrushes.GhostButton("+ Add speaker");
    private readonly TextBlock _speakers = Note("");
    private string _styleId = "", _stylesSignature = "", _styleTemplateKey = "";
    private bool _packMotion;

    private static ToggleButton Segment(string label, string id) => new() {
        Content = label, Tag = id, FontSize = 12, MinWidth = 0, Padding = new Thickness(10, 4, 10, 4), HorizontalAlignment = HorizontalAlignment.Stretch };

    // ---- layout pieces ---------------------------------------------------------------------
    private FrameworkElement PreviewHeader()
    {
        var header = new Grid { ColumnSpacing = 6 };
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(Step("3", "PREVIEW"));
        var transport = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center };
        transport.Children.Add(_playIn); transport.Children.Add(_playOutButton); transport.Children.Add(_loop);
        AutomationProperties.SetName(_playIn, "Play in"); AutomationProperties.SetName(_playOutButton, "Play out"); AutomationProperties.SetName(_loop, "Loop preview");
        ToolTipService.SetToolTip(_playIn, "Play the entrance in the private preview, at the chosen speed. Nothing goes on air.");
        ToolTipService.SetToolTip(_playOutButton, "Play the exit in the private preview.");
        ToolTipService.SetToolTip(_loop, "Loop in, hold, out in the private preview until you turn it off.");
        Grid.SetColumn(transport, 1); header.Children.Add(transport);
        return header;
    }

    private FrameworkElement BuildPackPlacement()
    {
        _elements.Children.Add(new TextBlock { Text = "OPTIONAL ELEMENTS", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted, Margin = new Thickness(0, 6, 0, 0) });
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 }; row.Children.Add(_secondLine); row.Children.Add(_tagLine);
        _elements.Children.Add(row);
        ToolTipService.SetToolTip(_secondLine, "Switch to this style's two-line look. Your text is kept.");
        ToolTipService.SetToolTip(_tagLine, "Switch to this style's look with a tag (LIVE, Speaker, Guest...).");

        _packPlacement.Children.Add(new TextBlock { Text = "POSITION", FontSize = 10, CharacterSpacing = 160, FontFamily = StudioBrushes.Mono, Foreground = StudioBrushes.Muted });
        _packPlacement.Children.Add(SegmentRow(_positionButtons));
        var sizeRow = new Grid { ColumnSpacing = 8 };
        sizeRow.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); sizeRow.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        var sizeLabel = new TextBlock { Text = "Size", FontSize = 12, Foreground = StudioBrushes.Text, VerticalAlignment = VerticalAlignment.Center };
        sizeRow.Children.Add(sizeLabel); var sizes = SegmentRow(_sizeButtons); Grid.SetColumn(sizes, 1); sizeRow.Children.Add(sizes);
        _packPlacement.Children.Add(sizeRow);
        _packPlacement.Children.Add(_safeArea);
        ToolTipService.SetToolTip(_safeArea, "Keep the graphic inside graphics-safe (x 96-1824, y 54-1026). A lower third sits on the band ending at y 1000.");
        _packPlacement.Children.Add(Note("Drag the preview to nudge. Anything else: Open in Designer."));
        return _packPlacement;
    }

    private static Grid SegmentRow(ToggleButton[] buttons)
    {
        var grid = new Grid { ColumnSpacing = 4 };
        for (var i = 0; i < buttons.Length; i++) {
            grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(buttons[i], i); grid.Children.Add(buttons[i]);
            AutomationProperties.SetName(buttons[i], buttons[i].Content?.ToString() ?? "");
        }
        return grid;
    }

    private void WirePackControls()
    {
        _playIn.Click += (_, _) => Play("in");
        _playOutButton.Click += (_, _) => Play("out");
        _loop.Click += (_, _) => { if (_loop.IsChecked == true) Play("loop"); else _session?.StopPreviewPlay(); };
        _secondLine.Click += (_, _) => ApplyElements(secondLineClicked: true);
        _tagLine.Click += (_, _) => ApplyElements(secondLineClicked: false);
        foreach (var button in _positionButtons) button.Click += (_, _) => ApplyPackPlacement(position: (string)button.Tag);
        foreach (var button in _sizeButtons) button.Click += (_, _) => ApplyPackPlacement(size: (string)button.Tag);
        _safeArea.Click += (_, _) => { if (_safeArea.IsChecked == true) SnapToSafeArea(force: true); _dirty = true; RefreshSaveButton(); };
        _speed.SelectionChanged += (_, _) => { if (!_loading && _session?.PreviewPlay is { Mode: "loop" }) Play("loop"); };
        _addSpeaker.Click += (_, _) => AddSpeaker();
        ToolTipService.SetToolTip(_addSpeaker, "Add the name and title typed above as one more speaker on this row (same design). The row's hotkey steps through them.");
        ToolTipService.SetToolTip(_saveCue, "Save to the show, then cue it on Preview. Program is unchanged until you Take or Show.");
        AutomationProperties.SetName(_saveCue, "Save and Cue");
    }

    // ---- style strip -----------------------------------------------------------------------
    private void RefreshStyles(GraphicsBuilderKind kind, IReadOnlyList<GraphicsBuilderPack> packs, bool hasClassic)
    {
        var signature = kind.Id + "|" + string.Join(",", packs.Select(p => p.Id + ":" + p.Label)) + "|" + hasClassic;
        if (signature != _stylesSignature) {
            _stylesSignature = signature; _styles.Children.Clear();
            var entries = packs.Select(p => (p.Id, p.Label, Tip: $"{p.Name}: {p.Templates.Count} look{(p.Templates.Count == 1 ? "" : "s")} for a {kind.Label.ToLowerInvariant()}")).ToList();
            if (hasClassic) entries.Add((GraphicsBuilder.ClassicStyleId, "Classic", "Built-in and imported looks that are not in a style pack"));
            foreach (var (id, label, tip) in entries) {
                var chip = new ToggleButton { Content = label, Tag = id, FontSize = 12, MinWidth = 0, Padding = new Thickness(6, 3, 6, 3), Margin = new Thickness(0, 0, 4, 4),
                    HorizontalAlignment = HorizontalAlignment.Stretch, VerticalAlignment = VerticalAlignment.Stretch };
                AutomationProperties.SetName(chip, "Style " + label);
                ToolTipService.SetToolTip(chip, tip);
                chip.Click += (_, _) => ChooseStyle(id);
                _styles.Children.Add(chip);
            }
            _styles.Visibility = entries.Count > 1 || packs.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        }
        foreach (var chip in _styles.Children.OfType<ToggleButton>()) chip.IsChecked = chip.Tag as string == _styleId;
    }

    private void ChooseStyle(string id)
    {
        if (_session is not { Busy: false } session) return;
        _styleId = id;
        var kind = Kind; var current = session.Template;
        var pack = GraphicsBuilder.PacksFor(kind, session.State).FirstOrDefault(p => p.Id == id);
        NativeGraphicsTemplate? look = null;
        if (pack is not null) {
            // Same optional elements in the new style when it has them.
            look = current is not null && GraphicsBuilder.IsPlainLowerThird(current)
                ? GraphicsBuilder.Sibling(pack, GraphicsBuilder.FirstLookOf(pack, kind), GraphicsBuilder.HasSecondLine(current), GraphicsBuilder.HasTag(current)) : null;
            look ??= GraphicsBuilder.FirstLookOf(pack, kind);
        } else if (GraphicsBuilder.StyleOf(current) != GraphicsBuilder.ClassicStyleId) look = GraphicsBuilder.ClassicTemplatesFor(kind, session.State).FirstOrDefault();
        if (look is not null && (current is null || look.TemplateId != current.TemplateId || look.TemplateVersion != current.TemplateVersion)) ChooseLook(look);
        else { _looksSignature = ""; RefreshLooks(); }
    }

    // ---- knobs -----------------------------------------------------------------------------
    private GraphicsBuilderPack? CurrentPack => _session?.Template is { Pack: not null } t
        ? GraphicsBuilder.PacksFor(Kind, _session.State).FirstOrDefault(p => p.Id == t.Pack!.Id) : null;

    /// <summary>Swaps the Motion / Speed choices when the look moves between a pack and a classic look.</summary>
    private void SyncMotionItems()
    {
        var pack = _session?.Template?.Pack is not null;
        if (pack == _packMotion && _motion.ItemsSource is not null) return;
        _packMotion = pack;
        var wasLoading = _loading; _loading = true;
        try {
            _motion.ItemsSource = pack ? new[] { "Pack default" }.Concat(GraphicsBuilder.MotionPresets.Select(m => m.Id == "default" ? "Fade" : m.Label)).ToArray()
                : GraphicsBuilder.MotionPresets.Select(m => m.Label).ToArray();
            _speed.ItemsSource = pack ? GraphicsBuilder.SpeedChoices.Select(c => c.Label).ToArray() : GraphicsBuilder.MotionSpeeds.Select(m => m.Label).ToArray();
            SetMotion(null);
        } finally { _loading = wasLoading; }
        ToolTipService.SetToolTip(_motion, pack ? "Pack default plays this style's own choreography. Shown on Program when you Show it; Play in previews it here."
            : "How the graphic comes in and goes out on air. Shown on Program when you Show it; Play in previews it here.");
    }

    private void RefreshPackControls()
    {
        var template = _session?.Template; var pack = template?.Pack is not null && Kind.Id != GraphicsBuilder.SponsorKindId;
        _packPlacement.Visibility = pack ? Visibility.Visible : Visibility.Collapsed;
        _placementRow.Visibility = _placementNote.Visibility = pack ? Visibility.Collapsed : Visibility.Visible;
        var family = CurrentPack;
        var plain = pack && family is not null && template is not null && GraphicsBuilder.IsPlainLowerThird(template);
        _elements.Visibility = plain ? Visibility.Visible : Visibility.Collapsed;
        if (plain) {
            var two = GraphicsBuilder.HasSecondLine(template!); var tag = GraphicsBuilder.HasTag(template!);
            _secondLine.IsChecked = two; _tagLine.IsChecked = tag;
            _secondLine.IsEnabled = ToggledLook(family!, template!, secondLine: true) is not null;
            _tagLine.IsEnabled = ToggledLook(family!, template!, secondLine: false) is not null;
        }
        if (pack && EffectivePlacement() is { } placement) {
            var position = GraphicsBuilder.PositionOf(placement); var size = GraphicsBuilder.SizeOf(placement);
            foreach (var b in _positionButtons) b.IsChecked = (string)b.Tag == position;
            foreach (var b in _sizeButtons) b.IsChecked = (string)b.Tag == size;
        }
        _addSpeaker.Visibility = Kind.Family == "lower-third" && template?.Fields.Any(f => f.Id == "name") == true ? Visibility.Visible : Visibility.Collapsed;
        var speakers = GraphicsBuilder.Speakers(_names.Text, NamesVariableIds());
        _speakers.Text = speakers.Count == 0 ? "" : "Speakers: " + string.Join(" · ", speakers);
        _speakers.Visibility = speakers.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        RefreshTransport();
    }

    /// <summary>Second line / Tag: the sibling look in the same pack, keeping the typed text (spec 5 optional elements).</summary>
    private void ApplyElements(bool secondLineClicked)
    {
        if (_session is not { Busy: false, Template: { } current } || CurrentPack is not { } pack) return;
        if (ToggledLook(pack, current, secondLineClicked) is { } next && next != current) ChooseLook(next);
        else RefreshPackControls();
    }

    /// <summary>The look after flipping Second line or Tag. In packs a tag rides with the second line, so turning
    /// the tag on adds the second line and turning the second line off drops the tag when no other look fits.</summary>
    private static NativeGraphicsTemplate? ToggledLook(GraphicsBuilderPack pack, NativeGraphicsTemplate current, bool secondLine)
    {
        var two = GraphicsBuilder.HasSecondLine(current); var tag = GraphicsBuilder.HasTag(current);
        if (secondLine) two = !two; else tag = !tag;
        return GraphicsBuilder.Sibling(pack, current, two, tag)
            ?? (secondLine ? GraphicsBuilder.Sibling(pack, current, two, false) : GraphicsBuilder.Sibling(pack, current, true, tag));
    }

    private void ApplyPackPlacement(string? position = null, string? size = null)
    {
        if (_session is not { Busy: false, Template: { } template }) return;
        var current = EffectivePlacement();
        position ??= GraphicsBuilder.PositionOf(current); size ??= GraphicsBuilder.SizeOf(current);
        var scale = GraphicsBuilder.SizeChoices.First(c => c.Id == size).Scale;
        _session.SetPlacement(GraphicsBuilder.PlaceInSafeArea(template, position, scale, _safeArea.IsChecked == true));
        _dirty = true; RefreshPackControls(); RefreshSaveButton();
    }

    /// <summary>Safe area on: a placement outside graphics-safe (a drag, an old row) is pulled back in. Never mid-drag.</summary>
    private void SnapToSafeArea(bool force = false)
    {
        if (_snapping || _session is not { Busy: false, Template: { Pack: not null } template, Draft.Placement: { IsValid: true } placement } || _safeArea.IsChecked != true ||
            !GraphicsBuilder.SnapsToSafeArea(template) || (_canvas.IsDragging && !force)) return;
        var (w, h) = GraphicsBuilder.BoundsOf(template);
        if (GraphicsBuilder.InsideSafeArea(placement, w, h)) return;
        _snapping = true;
        try { _session.SetPlacement(GraphicsBuilder.ClampToSafeArea(placement, w, h)); } finally { _snapping = false; }
    }
    private bool _snapping;

    // ---- preview transport ----------------------------------------------------------------
    private void Play(string mode)
    {
        if (_session is not { Busy: false, Draft: not null, Template: { } template } session) return;
        // The preview plays what Program will: this look's motion at the chosen speed.
        session.SetAnimations(GraphicsBuilder.EffectiveMotion(template, SelectedMotion, SelectedSpeed, GraphicsPlayoutService.Motion));
        session.PlayPreview(mode);
        RefreshTransport();
    }

    private void RefreshTransport()
    {
        var can = _session is { Draft: not null, Template: not null } && Kind.Id != GraphicsBuilder.SponsorKindId;
        _playIn.IsEnabled = _playOutButton.IsEnabled = _loop.IsEnabled = can;
        var looping = _session?.PreviewPlay is { Mode: "loop" };
        if (_loop.IsChecked != looping) _loop.IsChecked = looping;
    }

    // ---- palette, catalog, cue -------------------------------------------------------------
    private IReadOnlyDictionary<string, string>? CurrentPalette() => _session?.Palette?.Resolve();

    private async Task ListCatalogAsync(GraphicsWorkspaceSession session, IReadOnlyDictionary<string, string>? palette)
    {
        try { if (!await session.RefreshCatalogAsync(palette)) _thumbnailEpoch = ""; }
        catch (Exception ex) when (ex is not OutOfMemoryException) { _thumbnailEpoch = ""; }
    }

    private static async Task<string> CueSavedRowAsync(StudioViewModel studio, GraphicsPlaylistRow row)
    {
        try {
            if (GraphicsPlayoutActions.ResolveEntry(studio, row.Id, null) is not { } entry)
                return $"Saved \"{row.Name}\". Nothing to cue yet: {(row.FollowSources ? "no guest is assigned to a Show Input" : "the row has no entry")}.";
            var result = await studio.GraphicsPlayout.ToPreviewAsync(entry);
            return result.Accepted ? $"Saved and cued \"{row.Name}\" on Preview. Take or Show puts it on Program." : $"Saved \"{row.Name}\"; not cued: {result.Reason}";
        } catch (Exception ex) when (ex is not OutOfMemoryException) { return $"Saved \"{row.Name}\"; not cued: {ex.Message}"; }
    }

    // ---- variants -------------------------------------------------------------------------
    private void AddSpeaker()
    {
        if (_session?.Draft is not { } draft || draft.Fields.ValueKind != System.Text.Json.JsonValueKind.Object) return;
        string Field(string id) => draft.Fields.TryGetProperty(id, out var v) && v.ValueKind == System.Text.Json.JsonValueKind.String ? v.GetString() ?? "" : "";
        var name = Field("name");
        if (name.Trim().Length == 0) { _status.Text = "Type the speaker's name first."; return; }
        var before = GraphicsBuilder.Speakers(_names.Text, NamesVariableIds()).Count;
        var next = GraphicsBuilder.AddSpeaker(_names.Text, name, Field("title"), Field("tag"), NamesVariableIds());
        if (GraphicsBuilder.Speakers(next, NamesVariableIds()).Count == before) { _status.Text = "The names list is full (64)."; return; }
        _names.Text = next; _dirty = true;
        _status.Text = $"Added {name.Trim()}. Type the next speaker's name and press Add speaker again; Save keeps one row that steps through them.";
        RefreshNamesNote(); RefreshPackControls(); RefreshSaveButton();
    }
}
