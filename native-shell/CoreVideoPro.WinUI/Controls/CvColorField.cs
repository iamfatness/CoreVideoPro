using System.Runtime.InteropServices.WindowsRuntime;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>What a colour field committed: a literal colour (<see cref="Token"/> null), a token
/// binding (<see cref="Token"/> set, <see cref="WholeShow"/> false), or a change to that token for
/// the whole show (<see cref="WholeShow"/> true; the host saves the palette).</summary>
public sealed record CvColorCommit(string Value, string? Token, bool WholeShow);

/// <summary>The one colour control (graphics styles spec 4.2, slice 2). A swatch and a hex box
/// (#RGB, #RRGGBB or #RRGGBBAA) inline; the swatch opens a picker with the spectrum, R G B and
/// alpha fields, the show palette and the last twelve colours, plus the non-blocking contrast and
/// broadcast warnings (<see cref="GraphicsColorChecks"/>). When tokens are offered, a chip shows the
/// linked token (pick another or Detach); editing a linked colour asks "Change it for the whole
/// show" or "Detach just this". Picker edits are staged and committed once when the flyout closes,
/// so a drag never writes a document per pixel. Rows are built once and updated in place
/// (docs/reference/winui-crash-class-0xc000027b.md).</summary>
public sealed class CvColorField : UserControl
{
    /// <summary>The resolved show palette (token -> #RRGGBBAA) the swatch row offers. Set once by the shell.</summary>
    public static Func<IReadOnlyDictionary<string, string>>? PaletteProvider { get; set; }
    /// <summary>The eyedropper's core sampler (graphics styles slice 3). Null hides "Pick from Program".</summary>
    public static IColourSampler? Sampler { get; set; }

    private readonly TextBlock _header = new() { FontSize = 12, Visibility = Visibility.Collapsed };
    private readonly Border _chip = new() { Width = 22, Height = 22, CornerRadius = new CornerRadius(4), BorderThickness = new Thickness(1) };
    private readonly Button _swatch;
    private readonly TextBox _hex = new() { FontSize = 12, MaxLength = 9, MinWidth = 96, VerticalAlignment = VerticalAlignment.Center };
    private readonly DropDownButton _token = new() { FontSize = 11, Padding = new Thickness(8, 2, 8, 2), Visibility = Visibility.Collapsed, VerticalAlignment = VerticalAlignment.Center };
    private readonly StackPanel _warnings = new() { Spacing = 2 };
    private readonly Flyout _flyout = new() { Placement = FlyoutPlacementMode.Bottom };
    private readonly ColorPicker _picker = new() { IsColorChannelTextInputVisible = false, IsHexInputVisible = false, IsMoreButtonVisible = false,
        IsColorSliderVisible = true, IsColorPreviewVisible = true, MaxWidth = 300 };
    private readonly NumberBox _r = Channel("R"), _g = Channel("G"), _b = Channel("B"), _alpha = Channel("Alpha %", 100);
    private readonly TextBox _flyoutHex = new() { Header = "Hex", FontSize = 12, MaxLength = 9 };
    private readonly CvSwatchPanel _palette = new(), _recent = new();
    private readonly StackPanel _flyoutWarnings = new() { Spacing = 2 };
    private readonly StackPanel _linkChoice = new() { Spacing = 6, Visibility = Visibility.Collapsed };
    private readonly TextBlock _linkText = new() { FontSize = 11, TextWrapping = TextWrapping.WrapWholeWords };
    private readonly Button _wholeShow, _detach;
    private readonly Button _pickProgram;
    private readonly Image _frozen = new() { Stretch = Stretch.Uniform, Width = 320, Visibility = Visibility.Collapsed };
    private readonly TextBlock _pickStatus = new() { FontSize = 11, TextWrapping = TextWrapping.WrapWholeWords, Visibility = Visibility.Collapsed };
    private ColourSampleFrame? _frozenFrame;
    private bool _picking;
    private GraphicsRgba _value = new(255, 255, 255), _staged;
    private string? _boundToken;
    private bool _loading, _choiceMade;
    private IReadOnlyList<(string Id, string Label)> _tokens = [];

    public event EventHandler<CvColorCommit>? Committed;

    public CvColorField()
    {
        var root = new StackPanel { Spacing = 4 };
        root.Children.Add(_header);
        var row = new Grid { ColumnSpacing = 6 };
        row.ColumnDefinitions.Add(new() { Width = GridLength.Auto }); row.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); row.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        _chip.BorderBrush = StudioBrushes.Border;
        _swatch = StudioBrushes.GhostButton(""); _swatch.Content = _chip; _swatch.Padding = new Thickness(4); _swatch.Flyout = _flyout;
        row.Children.Add(_swatch);
        Grid.SetColumn(_hex, 1); row.Children.Add(_hex);
        Grid.SetColumn(_token, 2); row.Children.Add(_token);
        root.Children.Add(row); root.Children.Add(_warnings);
        Content = root;

        _hex.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter) CommitHex(_hex.Text); };
        _hex.LostFocus += (_, _) => CommitHex(_hex.Text);

        var body = new StackPanel { Spacing = 8, Width = 320 };
        body.Children.Add(_picker);
        var channels = new Grid { ColumnSpacing = 6 };
        var boxes = new[] { _r, _g, _b, _alpha };
        for (var i = 0; i < boxes.Length; i++) { channels.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); Grid.SetColumn(boxes[i], i); channels.Children.Add(boxes[i]); }
        body.Children.Add(_flyoutHex); body.Children.Add(channels);
        body.Children.Add(Caption("Palette")); body.Children.Add(_palette);
        body.Children.Add(Caption("Recent")); body.Children.Add(_recent);
        _pickProgram = StudioBrushes.GhostButton("\u2316 Pick from Program");
        ToolTipService.SetToolTip(_pickProgram, "Freeze the Program thumbnail, then click a pixel in it. Nothing on air changes.");
        AutomationProperties.SetName(_pickProgram, "Pick from Program");
        _pickProgram.Click += async (_, _) => await FreezeProgramAsync();
        _frozen.PointerPressed += async (_, e) => await PickAtAsync(e.GetCurrentPoint(_frozen).Position);
        AutomationProperties.SetName(_frozen, "Frozen Program frame");
        body.Children.Add(_pickProgram); body.Children.Add(_frozen); body.Children.Add(_pickStatus);
        body.Children.Add(_flyoutWarnings);
        _wholeShow = StudioBrushes.AccentButton("Change for the whole show");
        _detach = StudioBrushes.GhostButton("Detach just this");
        var choiceButtons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        choiceButtons.Children.Add(_wholeShow); choiceButtons.Children.Add(_detach);
        _linkChoice.Children.Add(_linkText); _linkChoice.Children.Add(choiceButtons);
        body.Children.Add(_linkChoice);
        _flyout.Content = new ScrollViewer { Content = body, MaxHeight = 640, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };

        _picker.ColorChanged += (_, e) => { if (!_loading) Stage(new GraphicsRgba(e.NewColor.R, e.NewColor.G, e.NewColor.B, AllowAlpha ? e.NewColor.A : (byte)255)); };
        foreach (var box in boxes) box.ValueChanged += (_, _) => {
            if (_loading || double.IsNaN(_r.Value) || double.IsNaN(_g.Value) || double.IsNaN(_b.Value)) return;
            var next = new GraphicsRgba((byte)Math.Clamp(_r.Value, 0, 255), (byte)Math.Clamp(_g.Value, 0, 255), (byte)Math.Clamp(_b.Value, 0, 255), _staged.A);
            Stage(AllowAlpha && !double.IsNaN(_alpha.Value) ? next.WithAlphaPercent(_alpha.Value) : next);
        };
        _flyoutHex.KeyDown += (_, e) => { if (e.Key == Windows.System.VirtualKey.Enter && Parse(_flyoutHex.Text) is { } parsed) Stage(parsed); };
        _flyoutHex.LostFocus += (_, _) => { if (Parse(_flyoutHex.Text) is { } parsed) Stage(parsed); };
        _wholeShow.Click += (_, _) => { if (_boundToken is { } token) { _choiceMade = true; Raise(new(_staged.Format(true), token, true)); _flyout.Hide(); } };
        _detach.Click += (_, _) => { _choiceMade = true; CommitLiteral(_staged); _flyout.Hide(); };
        _flyout.Opening += (_, _) => { _choiceMade = false; _staged = _value; _pickProgram.Visibility = Sampler is null ? Visibility.Collapsed : Visibility.Visible; RefreshFlyout(); };
        _flyout.Closed += (_, _) => _ = ReleaseFrozenAsync();
        _flyout.Closed += (_, _) => {
            if (_choiceMade || _staged == _value) return;
            // A linked colour needs the operator's choice; closing without one keeps the link and its colour.
            if (_boundToken is not null) { _staged = _value; Refresh(); return; }
            CommitLiteral(_staged);
        };
        AllowAlpha = true;
    }

    public string Header { get => _header.Text; set { _header.Text = value; _header.Visibility = value.Length == 0 ? Visibility.Collapsed : Visibility.Visible; AutomationProperties.SetName(_hex, value + " hex"); AutomationProperties.SetName(_swatch, "Pick " + value); } }
    /// <summary>False for targets that store opaque #RRGGBB (row appearances, the starter accent).</summary>
    private bool _allowAlpha = true;
    public bool AllowAlpha { get => _allowAlpha; set { _allowAlpha = value; _picker.IsAlphaEnabled = value; _picker.IsAlphaSliderVisible = value; _alpha.Visibility = value ? Visibility.Visible : Visibility.Collapsed; Refresh(); } }
    /// <summary>Tokens this target may link to (empty = literal colours only).</summary>
    public IReadOnlyList<(string Id, string Label)> Tokens { get => _tokens; set { _tokens = value; BuildTokenMenu(); Refresh(); } }
    /// <summary>Whether a linked colour may be changed for the whole show from here (the host saves the palette).</summary>
    public bool AllowWholeShowChange { get; set; } = true;
    private GraphicsColorCheckContext _context = GraphicsColorCheckContext.None;
    public GraphicsColorCheckContext CheckContext { get => _context; set { _context = value; Refresh(); } }
    public string Value => _boundToken ?? _value.Format(AllowAlpha);
    public string Literal => _value.Format(AllowAlpha);
    public string? Token => _boundToken;

    /// <summary>Show a value without raising <see cref="Committed"/>: a literal colour, or a token
    /// (its colour comes from <see cref="PaletteProvider"/>, else <paramref name="resolved"/>).</summary>
    public void SetValue(string value, string? resolved = null)
    {
        if (_tokens.Any(t => t.Id == value)) { _boundToken = value; _value = Parse(resolved ?? ResolveToken(value) ?? "") ?? _value; }
        else { _boundToken = null; if (Parse(value) is { } parsed) _value = AllowAlpha ? parsed : parsed with { A = 255 }; }
        _staged = _value; Refresh();
    }

    /// <summary>Commit typed text exactly as the hex box does (probes and automation use this).</summary>
    public void CommitText(string text) => CommitHex(text);

    private string? ResolveToken(string token) => PaletteProvider?.Invoke() is { } palette && palette.TryGetValue(token, out var value) ? value : null;
    private GraphicsRgba? Parse(string text) => GraphicsRgba.TryParse(text, out var value) ? AllowAlpha ? value : value with { A = 255 } : null;

    private void CommitHex(string text)
    {
        if (_loading) return;
        if (text.Trim().Length == 0 || string.Equals(text.Trim(), _value.Format(AllowAlpha), StringComparison.OrdinalIgnoreCase)) { Refresh(); return; }
        if (Parse(text) is not { } parsed) { ShowInline([new("input", "Use #RGB, #RRGGBB" + (AllowAlpha ? " or #RRGGBBAA." : "."))]); return; }
        if (_boundToken is not null) { _staged = parsed; _flyout.ShowAt(_swatch); return; } // ask whole show / detach
        CommitLiteral(parsed);
    }
    private void CommitLiteral(GraphicsRgba value)
    {
        _boundToken = null; _value = value; _staged = value;
        GraphicsColorRecents.Shared.Add(value.Hex8);
        Refresh(); Raise(new(value.Format(AllowAlpha), null, false));
    }
    private void Bind(string token)
    {
        _boundToken = token; _value = Parse(ResolveToken(token) ?? "") ?? _value; _staged = _value;
        Refresh(); Raise(new(_value.Format(true), token, false));
    }
    private void Raise(CvColorCommit commit)
    {
        if (commit.WholeShow) { _value = _staged; GraphicsColorRecents.Shared.Add(_staged.Hex8); Refresh(); }
        Committed?.Invoke(this, commit);
    }

    private void Stage(GraphicsRgba value)
    {
        _staged = value; RefreshFlyout();
        _chip.Background = Brush(value);
    }

    private void BuildTokenMenu()
    {
        var menu = new MenuFlyout();
        foreach (var (id, label) in _tokens) { var item = new MenuFlyoutItem { Text = label }; item.Click += (_, _) => Bind(id); menu.Items.Add(item); }
        if (_tokens.Count > 0) {
            menu.Items.Add(new MenuFlyoutSeparator());
            var detach = new MenuFlyoutItem { Text = "Detach (keep this colour)" };
            detach.Click += (_, _) => { if (_boundToken is not null) CommitLiteral(_value); };
            menu.Items.Add(detach);
        }
        _token.Flyout = menu;
        _token.Visibility = _tokens.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Refresh()
    {
        _loading = true;
        try {
            _chip.Background = Brush(_value);
            if (_hex.FocusState == FocusState.Unfocused) _hex.Text = _value.Format(AllowAlpha);
            _hex.PlaceholderText = AllowAlpha ? "#RRGGBBAA" : "#RRGGBB";
            var label = _boundToken is null ? "Not linked" : _tokens.FirstOrDefault(t => t.Id == _boundToken).Label ?? _boundToken;
            _token.Content = _boundToken is null ? "● Detached" : "● " + label;
            ToolTipService.SetToolTip(_token, _boundToken is null ? "This colour is not linked to the palette. Pick a token to link it." : $"Linked to {_boundToken}. Pick another token or Detach.");
            ShowInline(GraphicsColorChecks.Evaluate(_value.Hex8, CheckContext));
        } finally { _loading = false; }
    }

    private void RefreshFlyout()
    {
        _loading = true;
        try {
            _picker.Color = Windows.UI.Color.FromArgb(_staged.A, _staged.R, _staged.G, _staged.B);
            _r.Value = _staged.R; _g.Value = _staged.G; _b.Value = _staged.B; _alpha.Value = _staged.AlphaPercent;
            if (_flyoutHex.FocusState == FocusState.Unfocused) _flyoutHex.Text = _staged.Format(AllowAlpha);
            FillSwatches(_palette, (PaletteProvider?.Invoke() ?? new Dictionary<string, string>()).Select(p => (p.Value, (string?)p.Key)));
            FillSwatches(_recent, GraphicsColorRecents.Shared.Items.Select(v => (v, (string?)null)));
            Fill(_flyoutWarnings, GraphicsColorChecks.Evaluate(_staged.Hex8, CheckContext));
            var pending = _boundToken is not null && _staged != _value;
            _linkChoice.Visibility = pending ? Visibility.Visible : Visibility.Collapsed;
            _wholeShow.Visibility = AllowWholeShowChange ? Visibility.Visible : Visibility.Collapsed;
            _linkText.Text = pending ? $"This colour is linked to {_boundToken}. Change {_boundToken} for the whole show, or detach just this one?" : "";
        } finally { _loading = false; }
    }

    private void FillSwatches(CvSwatchPanel panel, IEnumerable<(string Colour, string? Token)> items)
    {
        // Twelve small buttons at most per row set; rebuilt only while the flyout is open, never per frame.
        panel.Children.Clear();
        foreach (var (colour, token) in items.Take(GraphicsColorRecents.Capacity)) {
            if (!GraphicsRgba.TryParse(colour, out var value)) continue;
            var button = new Button { Width = 22, Height = 22, Padding = new Thickness(0), Margin = new Thickness(0, 0, 4, 4), Background = Brush(value),
                BorderBrush = StudioBrushes.Border, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(4) };
            var name = token is null ? value.Hex8 : $"{token} {value.Hex8}";
            ToolTipService.SetToolTip(button, name); AutomationProperties.SetName(button, name);
            button.Click += (_, _) => {
                _choiceMade = true;
                if (token is not null && _tokens.Any(t => t.Id == token)) Bind(token); else CommitLiteral(AllowAlpha ? value : value with { A = 255 });
                _flyout.Hide();
            };
            panel.Children.Add(button);
        }
    }

    private void ShowInline(IReadOnlyList<GraphicsColorWarning> warnings) => Fill(_warnings, warnings);
    private void Fill(StackPanel panel, IReadOnlyList<GraphicsColorWarning> warnings)
    {
        panel.Children.Clear();
        foreach (var warning in warnings) {
            var line = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            line.Children.Add(new TextBlock { Text = "⚠ " + warning.Message, FontSize = 11, Foreground = StudioBrushes.Program, TextWrapping = TextWrapping.WrapWholeWords, MaxWidth = 300 });
            if (warning.FixValue is { } fix && warning.FixLabel is { } fixLabel) {
                var apply = StudioBrushes.ChipButton(fixLabel);
                apply.Click += (_, _) => { if (Parse(fix) is { } capped) { _choiceMade = true; if (_boundToken is null) CommitLiteral(capped); else { _staged = capped; _flyout.ShowAt(_swatch); } } };
                line.Children.Add(apply);
            }
            panel.Children.Add(line);
        }
    }

    // ---- eyedropper (slice 3) -----------------------------------------------------------
    /// <summary>Freezes the core's Program thumbnail and shows exactly those bytes; a click picks from them.</summary>
    private async Task FreezeProgramAsync()
    {
        if (Sampler is not { } sampler || _picking) return;
        _picking = true; _pickProgram.IsEnabled = false;
        try {
            var (frame, reason) = await sampler.FreezeAsync();
            _pickStatus.Text = reason; _pickStatus.Visibility = Visibility.Visible;
            _frozenFrame = frame;
            if (frame is null) { _frozen.Visibility = Visibility.Collapsed; return; }
            var bitmap = new WriteableBitmap(frame.Width, frame.Height);
            using (var stream = bitmap.PixelBuffer.AsStream()) stream.Write(frame.Bgra, 0, frame.Bgra.Length);
            bitmap.Invalidate();
            _frozen.Source = bitmap; _frozen.Height = 320.0 * frame.Height / frame.Width; _frozen.Visibility = Visibility.Visible;
        } catch (Exception ex) when (ex is InvalidOperationException or OperationCanceledException or IOException) {
            _pickStatus.Text = "Could not freeze Program: " + ex.Message; _pickStatus.Visibility = Visibility.Visible;
        } finally { _picking = false; _pickProgram.IsEnabled = true; }
    }

    private async Task PickAtAsync(Windows.Foundation.Point at)
    {
        if (Sampler is not { } sampler || _frozenFrame is not { } frame || _picking || _frozen.ActualWidth <= 0 || _frozen.ActualHeight <= 0) return;
        _picking = true;
        try {
            var (hex, reason) = await sampler.PickAsync(frame, Math.Clamp(at.X / _frozen.ActualWidth, 0, 1), Math.Clamp(at.Y / _frozen.ActualHeight, 0, 1));
            _pickStatus.Text = reason;
            if (hex is not null && Parse(hex) is { } picked) Stage(AllowAlpha ? picked with { A = _staged.A } : picked);
        } catch (Exception ex) when (ex is InvalidOperationException or OperationCanceledException or IOException) {
            _pickStatus.Text = "The pick failed: " + ex.Message;
        } finally { _picking = false; }
    }

    private async Task ReleaseFrozenAsync()
    {
        _frozen.Visibility = Visibility.Collapsed; _frozen.Source = null; _pickStatus.Visibility = Visibility.Collapsed;
        if (_frozenFrame is null || Sampler is not { } sampler) return;
        _frozenFrame = null;
        try { await sampler.ReleaseAsync(); } catch (Exception ex) when (ex is InvalidOperationException or OperationCanceledException or IOException) { /* The core drops it on restart. */ }
    }

    private static Brush Brush(GraphicsRgba value) => new SolidColorBrush(Windows.UI.Color.FromArgb(value.A, value.R, value.G, value.B));
    private static TextBlock Caption(string text) => new() { Text = text.ToUpperInvariant(), FontSize = 10, CharacterSpacing = 120, Foreground = StudioBrushes.Muted };
    private static NumberBox Channel(string header, double max = 255) => new() { Header = header, Minimum = 0, Maximum = max, FontSize = 12,
        SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Hidden, ValidationMode = NumberBoxValidationMode.InvalidInputOverwritten };
}

/// <summary>Minimal wrapping panel for the swatch rows.</summary>
public sealed class CvSwatchPanel : Panel
{
    protected override Windows.Foundation.Size MeasureOverride(Windows.Foundation.Size available)
    {
        double x = 0, y = 0, row = 0, width = double.IsInfinity(available.Width) ? 320 : available.Width;
        foreach (var child in Children) {
            child.Measure(available);
            if (x > 0 && x + child.DesiredSize.Width > width) { x = 0; y += row; row = 0; }
            x += child.DesiredSize.Width; row = Math.Max(row, child.DesiredSize.Height);
        }
        return new(width, y + row);
    }
    protected override Windows.Foundation.Size ArrangeOverride(Windows.Foundation.Size final)
    {
        double x = 0, y = 0, row = 0;
        foreach (var child in Children) {
            if (x > 0 && x + child.DesiredSize.Width > final.Width) { x = 0; y += row; row = 0; }
            child.Arrange(new(x, y, child.DesiredSize.Width, child.DesiredSize.Height));
            x += child.DesiredSize.Width; row = Math.Max(row, child.DesiredSize.Height);
        }
        return final;
    }
}
