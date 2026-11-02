using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>"Start a graphic" (spec §2, mockup D2): start blank from one of the four seeds, or from a
/// look (a design or an installed template — always copied, never edited in place), give it a name,
/// then "Open in the designer". A refusal keeps the dialog open with the reason in its status line.</summary>
public sealed class GraphicsDesignStartDialog : ContentDialog
{
    private static readonly IReadOnlyDictionary<string, (string Label, string Blurb)> KindText = new Dictionary<string, (string, string)>(StringComparer.Ordinal) {
        ["lower-third"] = ("Lower third", "A name and a title over the picture."),
        ["title"] = ("Title", "A heading and a subheading."),
        ["bug"] = ("Bug or corner", "A logo or tag in a corner."),
        ["full-screen"] = ("Full screen", "A full-frame card."),
    };

    private readonly StudioViewModel _studio;
    private readonly List<ToggleButton> _kinds = [];
    private readonly ComboBox _look = new() { Header = "Or start from a look", PlaceholderText = "Choose a look to copy", HorizontalAlignment = HorizontalAlignment.Stretch, FontSize = 14 };
    private readonly TextBox _name = new() { Header = "Name", MaxLength = GraphicsDesignIds.MaxNameLength, PlaceholderText = "Guest strap", FontSize = 14 };
    private readonly TextBlock _status = new() { FontSize = 14, TextWrapping = TextWrapping.Wrap };
    private readonly List<(string TemplateId, int Version, string Name)> _looks = [];
    private string _autoName = "";
    private bool _choosing;

    public GraphicsDesignStartDialog(StudioViewModel studio)
    {
        _studio = studio;
        Title = "New design";
        PrimaryButtonText = "Open in the designer"; CloseButtonText = "Cancel"; DefaultButton = ContentDialogButton.Primary;

        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock { Text = "Start blank", FontSize = 14, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Foreground = StudioBrushes.Text });
        var grid = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        grid.RowDefinitions.Add(new() { Height = GridLength.Auto }); grid.RowDefinitions.Add(new() { Height = GridLength.Auto });
        foreach (var (kind, index) in GraphicsDesignSeeds.Kinds.Select((k, i) => (k, i))) {
            var (label, blurb) = KindText.TryGetValue(kind, out var text) ? text : (kind, "");
            var content = new StackPanel { Spacing = 2 };
            content.Children.Add(new TextBlock { Text = label, FontSize = 15, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
            content.Children.Add(new TextBlock { Text = blurb, FontSize = 14, TextWrapping = TextWrapping.Wrap, Opacity = .8 });
            var tile = new ToggleButton { Content = content, Tag = kind, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left, Padding = new Thickness(12, 8, 12, 8), MinHeight = 64 };
            AutomationProperties.SetName(tile, "Start blank: " + label);
            tile.Checked += (_, _) => ChooseKind(tile, label);
            tile.Unchecked += (_, _) => { if (!_choosing) RefreshPrimary(); };
            Grid.SetRow(tile, index / 2); Grid.SetColumn(tile, index % 2); grid.Children.Add(tile); _kinds.Add(tile);
        }
        body.Children.Add(grid);

        foreach (var design in studio.GraphicsLibrary.Document.Designs) _looks.Add((design.TemplateId, design.TemplateVersion, "Your design · " + design.Name));
        foreach (var template in (studio.GraphicsWorkspace.State?.Templates ?? []).Where(t => t.Runtime == "declarative" && !GraphicsDesignIds.IsDesignTemplateId(t.TemplateId))
                     .GroupBy(t => t.TemplateId).Select(g => g.OrderByDescending(t => t.TemplateVersion).First()).OrderBy(t => t.Name, StringComparer.CurrentCultureIgnoreCase))
            _looks.Add((template.TemplateId, template.TemplateVersion, template.Name.Length > 0 ? template.Name : template.TemplateId));
        _look.ItemsSource = _looks.Select(l => l.Name).ToArray();
        _look.IsEnabled = _looks.Count > 0;
        if (_looks.Count == 0) _look.PlaceholderText = "No looks are available yet";
        AutomationProperties.SetName(_look, "Start from a look");
        _look.SelectionChanged += (_, _) => ChooseLook();
        body.Children.Add(_look);
        AutomationProperties.SetName(_name, "Design name");
        _name.TextChanged += (_, _) => RefreshPrimary();
        body.Children.Add(_name);
        body.Children.Add(_status);
        Content = new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };

        PrimaryButtonClick += OnPrimaryButtonClick;
        RefreshPrimary();
    }

    /// <summary>The successful create, or null when cancelled.</summary>
    public GraphicsDesignResult? Result { get; private set; }

    private string? SelectedKind => _kinds.FirstOrDefault(k => k.IsChecked == true)?.Tag as string;

    private void ChooseKind(ToggleButton chosen, string label)
    {
        if (_choosing) return;
        _choosing = true;
        try { foreach (var tile in _kinds) if (!ReferenceEquals(tile, chosen)) tile.IsChecked = false; _look.SelectedIndex = -1; }
        finally { _choosing = false; }
        SuggestName(label);
    }

    private void ChooseLook()
    {
        if (_choosing || _look.SelectedIndex < 0 || _look.SelectedIndex >= _looks.Count) { RefreshPrimary(); return; }
        _choosing = true;
        try { foreach (var tile in _kinds) tile.IsChecked = false; }
        finally { _choosing = false; }
        var look = _looks[_look.SelectedIndex].Name;
        SuggestName((look.StartsWith("Your design · ", StringComparison.Ordinal) ? look["Your design · ".Length..] : look) + " copy");
    }

    private void SuggestName(string suggestion)
    {
        if (suggestion.Length > GraphicsDesignIds.MaxNameLength) suggestion = suggestion[..GraphicsDesignIds.MaxNameLength].TrimEnd();
        if (_name.Text.Trim().Length == 0 || _name.Text == _autoName) { _autoName = suggestion; _name.Text = suggestion; }
        RefreshPrimary();
    }

    private void RefreshPrimary() =>
        IsPrimaryButtonEnabled = (SelectedKind is not null || _look.SelectedIndex >= 0) && _name.Text.Trim().Length > 0;

    private async void OnPrimaryButtonClick(ContentDialog sender, ContentDialogButtonClickEventArgs args)
    {
        var deferral = args.GetDeferral();
        try {
            var kind = SelectedKind;
            var look = kind is null && _look.SelectedIndex >= 0 && _look.SelectedIndex < _looks.Count ? _looks[_look.SelectedIndex] : default;
            if (kind is null && look.TemplateId is null) { _status.Text = "Choose a blank kind or a look to start from."; args.Cancel = true; return; }
            IsPrimaryButtonEnabled = false; _status.Text = "Creating the design…";
            var result = await _studio.CreateGraphicsDesignAsync(kind, look.TemplateId, look.Version, _name.Text);
            if (!result.Ok) { _status.Text = result.Message; args.Cancel = true; return; }
            Result = result;
        } catch (Exception ex) when (ex is not OutOfMemoryException) {
            _status.Text = "The design could not be created: " + ex.Message; args.Cancel = true;
        } finally { RefreshPrimary(); deferral.Complete(); }
    }
}
