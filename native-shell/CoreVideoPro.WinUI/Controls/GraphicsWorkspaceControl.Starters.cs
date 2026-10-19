using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsWorkspaceControl
{
    private readonly Grid _starterHome = new() { RowSpacing = 16, Padding = new Thickness(24) };
    private readonly Grid _starterCards = new() { ColumnSpacing = 16, RowSpacing = 16 };
    private readonly StackPanel _starterBanner = new() { Orientation = Orientation.Horizontal, Spacing = 12, Padding = new Thickness(12), Visibility = Visibility.Collapsed };
    private readonly ListView _starterGraphics = new() { SelectionMode = ListViewSelectionMode.Single, Visibility = Visibility.Collapsed, HorizontalContentAlignment = HorizontalAlignment.Stretch };
    private readonly TextBlock _starterNotice = Copy("");
    private readonly TextBlock _starterFeedback = Copy("");
    private readonly StackPanel _starterBrandTools = new() { Orientation=Orientation.Horizontal, Spacing=8, Visibility=Visibility.Collapsed };
    private readonly TextBox _starterBrandAccent = new() { Header="Brand color", Width=100, MaxLength=7 };
    private readonly TextBlock _starterHeading = new() { FontSize = 16, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _assignmentHelp = Copy("");
    private readonly List<Button> _starterChoices = [];
    private Button _addStarter = null!, _continueShow = null!;
    private GraphicsShowStarter? _activeStarter;
    private IReadOnlyList<GraphicsStarterGraphic> _starterDrafts = [];
    private bool _starterAdding, _starterLogoImporting, _starterHomeOpen = true;
    private bool _starterInitialSelectionApplied;
    private int _starterPreviewIndex = -1;
    private string _starterEpoch = "";

    private void BuildStarters(Grid root)
    {
        _starterHome.RowDefinitions.Add(new() { Height = GridLength.Auto });
        _starterHome.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        _starterHome.RowDefinitions.Add(new() { Height = GridLength.Auto });
        var welcome = new StackPanel { Spacing = 6 };
        welcome.Children.Add(new TextBlock { Text = "1. Choose a show look", FontSize = 26 });
        welcome.Children.Add(Copy("Choose a look → personalize its colors and logo → Use this look in Studio. Nothing goes on air while you experiment."));
        _starterHome.Children.Add(welcome);
        for (var i = 0; i < 2; i++) { _starterCards.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) }); _starterCards.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) }); }
        Grid.SetRow(_starterCards, 1); _starterHome.Children.Add(_starterCards);
        var footer = new StackPanel { Spacing = 8 };
        footer.Children.Add(_starterNotice);
        var options = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12 };
        _continueShow = ChromeButton("Continue my show"); _continueShow.Click += async (_, _) => await ContinueShowAsync();
        options.Children.Add(_continueShow);
        var individual = ChromeButton("Browse individual graphics"); individual.Click += (_, _) => OpenIndividualGraphics();
        options.Children.Add(individual); footer.Children.Add(options); Grid.SetRow(footer, 2); _starterHome.Children.Add(footer);
        Grid.SetRow(_starterHome, 1); root.Children.Add(_starterHome);

        var back = ChromeButton("← Show templates"); back.Click += (_, _) => OpenShowTemplates(); _starterBanner.Children.Add(back);
        _starterBanner.Children.Add(_starterHeading);
        _addStarter = ChromeButton("Save graphics only");
        _addStarter.Click += async (_, _) => await AddStarterToShowAsync(); _starterBanner.Children.Add(_addStarter);
        _starterBrandTools.Children.Add(_starterBrandAccent);
        _starterBrandAccent.TextChanged += (_,_) => ApplyValidStarterBranding();
        var color = ChromeButton("Pick color…"); color.VerticalAlignment=VerticalAlignment.Bottom;
        var picker = new ColorPicker { IsAlphaEnabled=false, Color=Windows.UI.Color.FromArgb(255,68,193,161), MaxWidth=300 };
        picker.ColorChanged += (_, e) => { if (!_starterAdding) _starterBrandAccent.Text=$"#{e.NewColor.R:X2}{e.NewColor.G:X2}{e.NewColor.B:X2}"; };
        color.Flyout=new Flyout {Content=picker};
        color.Flyout.Opening+=(_,_)=>{var hex=_starterBrandAccent.Text.Trim();if(hex.Length==7 && hex[0]=='#' && hex.Skip(1).All(Uri.IsHexDigit))picker.Color=Windows.UI.Color.FromArgb(255,Convert.ToByte(hex.Substring(1,2),16),Convert.ToByte(hex.Substring(3,2),16),Convert.ToByte(hex.Substring(5,2),16));};
        _starterBrandTools.Children.Add(color);
        var logo = ChromeButton("Use my logo…"); logo.VerticalAlignment=VerticalAlignment.Bottom; logo.Click+=async(_,_)=>await PickStarterLogoAsync(); _starterBrandTools.Children.Add(logo);
        Grid.SetRow(_starterBanner, 0); // Inserted by the workspace's preview header, below its title.
        _starterGraphics.SelectionChanged += (_, _) => {
            if (!_loading && !_starterAdding && _starterGraphics.SelectedIndex is var index && index >= 0 && index < _starterDrafts.Count) {
                RememberStarterEdits(); _starterPreviewIndex = index;
                _session?.PreviewStarter(_starterDrafts[index]); SelectInspectorTab(0);
            }
        };
        AutomationProperties.SetName(_starterGraphics,"Show template sample graphics");
        RefreshStarterCards(); RefreshStarterChrome();
    }

    private void OpenShowTemplates()
    {
        if (_starterAdding) return;
        _starterHomeOpen = true; _starterInitialSelectionApplied = true; _tools.IsPaneOpen = false; RefreshStarterCards(); RefreshStarterChrome();
    }

    private void OpenIndividualGraphics()
    {
        if (_starterAdding) return;
        RememberStarterEdits(); _activeStarter=null; _starterHomeOpen=false; _starterInitialSelectionApplied=true;
        RefreshStarterChrome(); OpenTools("library","Templates and saved cues");
    }

    private void RefreshStarterCards()
    {
        if (_starterCards.Children.Count > 0) return;
        foreach (var (starter, index) in GraphicsShowStarters.All.Select((s, i) => (s, i))) {
            var content = new StackPanel { Spacing = 8, HorizontalAlignment = HorizontalAlignment.Stretch };
            content.Children.Add(new TextBlock { Text = starter.Name, FontSize = 20 });
            content.Children.Add(Copy(starter.Description));
            var reference=new Image {Source=new BitmapImage(new Uri($"ms-appx:///Assets/GraphicsStarters/{starter.Id}.png")),Height=110,MaxWidth=640,Stretch=Stretch.Uniform};
            AutomationProperties.SetName(reference,"Show template reference: "+starter.Id);content.Children.Add(reference);
            content.Children.Add(Copy("Guest introduction · Welcome title · Show logo · Live clock"));
            content.Children.Add(new TextBlock { Text = "Try this template →", FontSize = 14 });
            var button = ChromeButton(starter.Name); button.Content = content; button.HorizontalAlignment = HorizontalAlignment.Stretch; button.VerticalAlignment = VerticalAlignment.Stretch; button.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            AutomationProperties.SetName(button, starter.Name);
            button.Click += (_, _) => TryShowStarter(starter);
            Grid.SetRow(button, index / 2); Grid.SetColumn(button, index % 2); _starterCards.Children.Add(button); _starterChoices.Add(button);
        }
    }

    private void TryShowStarter(GraphicsShowStarter starter)
    {
        if (_session is null || _session.Busy || _starterAdding) return;
        try {
            _starterDrafts = GraphicsShowStarters.Prepare(starter, _session.State ?? throw new InvalidOperationException("Graphics is starting. Try this template again in a moment."));
            _activeStarter = starter; _starterHomeOpen = false; _starterInitialSelectionApplied = true; _starterEpoch = _session.State!.CoreEpoch; _starterPreviewIndex = 0;
            _starterFeedback.Text = "";
            var loading = _loading; _loading = true;
            try {
                _starterBrandAccent.Text=starter.Accent;
                _starterGraphics.Items.Clear();
                foreach(var graphic in _starterDrafts) {
                    var row=new StackPanel {Spacing=3}; row.Children.Add(new TextBlock {Text=graphic.Name,TextWrapping=TextWrapping.Wrap});
                    row.Children.Add(new TextBlock {Text=graphic.Purpose,FontSize=11,TextWrapping=TextWrapping.Wrap,MaxLines=2});
                    _starterGraphics.Items.Add(row);
                }
                _starterGraphics.SelectedIndex = 0;
            }
            finally { _loading = loading; }
            _session.PreviewStarter(_starterDrafts[0]); SetGraphicsCustomization(false); RefreshStarterChrome();
        } catch (InvalidOperationException ex) { _starterNotice.Text = ex.Message; }
    }

    private async Task AddStarterToShowAsync(bool useInStudio = false)
    {
        if (_session is null || _activeStarter is null || _starterAdding || _starterLogoImporting) return;
        // Retain edits to the currently previewed sample when adopting the set.
        RememberStarterEdits();
        if (_savedLibrary is { ReadOnly: true }) { _status.Text = "Your Graphics document is read-only. Use a supported document before adding a show template."; return; }
        var session=_session;var library=_savedLibrary;
        _starterFeedback.Text=useInStudio?"Saving your look and assigning it to Studio…":"Saving graphics…";
        _starterAdding = true; RefreshStarterChrome();
        try {
            if(library is null){_starterFeedback.Text="The Graphics document is unavailable. Your private edits were kept.";return;}
            var result=await GraphicsStarterWorkflow.AddAsync(session,library,_starterDrafts,useInStudio,_studioFollowSource.IsChecked==true);
            _starterNotice.Text=_starterFeedback.Text=result.Message;
            if(result.Added)_activeStarter=null;
        } catch(Exception ex) when(ex is not OutOfMemoryException) {
            _starterFeedback.Text="The show template could not be completed: "+ex.Message+". Any added graphics remain hidden.";
        } finally { _starterAdding = false; Refresh(); RefreshStarterChrome(); }
    }

    private void RefreshStarterChrome()
    {
        if (!_starterInitialSelectionApplied && _session?.Selected is not null) { _starterInitialSelectionApplied = true; _starterHomeOpen = false; }
        if (_activeStarter is not null && _session?.State?.CoreEpoch != _starterEpoch && !_starterAdding) {
            _activeStarter = null; _starterHomeOpen = true; _starterNotice.Text = "The media core reconnected. Choose a show template to try it again; nothing was shown.";
        }
        _starterHome.Visibility = _starterHomeOpen ? Visibility.Visible : Visibility.Collapsed;
        _body.Visibility = _starterHomeOpen ? Visibility.Collapsed : Visibility.Visible;
        _transport.Visibility = _starterHomeOpen || _activeStarter is not null ? Visibility.Collapsed : Visibility.Visible;
        var trying = !_starterHomeOpen && _activeStarter is not null;
        _cueRackTitle.Text=trying?"Sample graphics":"Show cues";
        _cueSearch.Visibility=_cueScopeFilter.Visibility=trying?Visibility.Collapsed:Visibility.Visible;
        _starterBanner.Visibility = trying ? Visibility.Visible : Visibility.Collapsed;
        _starterBrandTools.Visibility = trying ? Visibility.Visible : Visibility.Collapsed;
        foreach(var control in _starterBrandTools.Children.OfType<Microsoft.UI.Xaml.Controls.Control>()) control.IsEnabled = !_starterAdding && !_starterLogoImporting;
        _cues.IsEnabled=_starterGraphics.IsEnabled=_samples.IsEnabled=_compare.IsEnabled=_canvas.IsEnabled=_customizeGraphic.IsEnabled=!_starterAdding;
        foreach(var scroll in _inspectorScrolls)scroll.IsEnabled=!_starterAdding;
        _starterHeading.Text = "2. Personalize · " + (_activeStarter?.Name ?? "");
        _starterGraphics.Visibility = trying ? Visibility.Visible : Visibility.Collapsed;
        _cues.Visibility = trying ? Visibility.Collapsed : Visibility.Visible;
        _continueShow.IsEnabled = !_starterAdding && (_session?.State?.Instances.Count > 0 || _savedLibrary?.Document.Cues.Count > 0);
        _addStarter.IsEnabled = !_starterAdding && !_starterLogoImporting && _session is { Busy: false };
        foreach (var choice in _starterChoices) choice.IsEnabled = !_starterAdding && _session is { Busy: false };
        _assignmentHelp.Text = trying ? "3. Use this look in Studio saves the set and assigns its guest-introduction look. Then Go to Studio and use LT in. Names come from the Program source when enabled." : _session?.Template?.Family != "lower-third" ? "Studio's lower thirds toggle uses a guest introduction. Select that graphic to assign its look." : _session?.Selected is null ? "Add this graphic to your show before assigning its look to Studio." : "Use in Studio assigns the look. Go to Studio, then use LT in when you want names on air.";
        _starterFeedback.Visibility = _starterFeedback.Text.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        RefreshWorkspaceChrome();
    }

    private void RememberStarterEdits()
    {
        var selected = _starterPreviewIndex;
        if (selected >= 0 && selected < _starterDrafts.Count && _session is { Selected: null, Draft: { } edited } &&
            edited.TemplateId == _starterDrafts[selected].Draft.TemplateId && edited.TemplateVersion == _starterDrafts[selected].Draft.TemplateVersion)
            _starterDrafts = _starterDrafts.Select((g, i) => i == selected ? g with { Draft = edited, Template = _session.Template ?? g.Template } : g).ToArray();
    }

    private async Task ContinueShowAsync()
    {
        if (_session is null || _starterAdding) return;
        var restored=false;
        _starterInitialSelectionApplied = true;
        if (_session.State?.Instances.Count == 0 && _savedLibrary is not null) {
            _starterAdding = true; RefreshStarterChrome();
            try {
                await _savedLibrary.LoadAsync();
                var saved = _savedLibrary.Document.Cues;
                if(saved.Count>8 || saved.Any(c=>c.SourceId.Length>0)) {
                    _starterNotice.Text="Your saved show needs a selection or current guest binding. Open Templates and saved cues to restore the graphics you want.";
                    _starterFeedback.Text=_starterNotice.Text; _activeStarter=null;
                    _starterHomeOpen=false;OpenTools("library","Your saved show");return;
                }
                foreach(var cue in saved) if(!await _savedLibrary.RestoreAsync(cue)) {_starterNotice.Text=_savedLibrary.Status;return;}
                restored=true;
                _starterFeedback.Text="Your saved graphics are restored hidden. Choose one to preview or show; automation remains unarmed.";
            } finally {_starterAdding=false;RefreshStarterChrome();}
        }
        _activeStarter = null; _starterHomeOpen = false;
        if ((restored || _session.Selected is null) && _session.State?.Instances.FirstOrDefault() is { } first) _session.Select(first.InstanceId);
        RefreshStarterChrome();
    }

    private void ApplyStarterBranding()
    {
        if (_session is null || _activeStarter is null || _starterAdding || _starterLogoImporting) return;
        RememberStarterEdits();
        try {_starterDrafts=GraphicsShowStarters.WithAccentColor(_starterDrafts,_starterBrandAccent.Text.Trim());}
        catch(InvalidOperationException ex) {_starterFeedback.Text=ex.Message;RefreshStarterChrome();return;}
        _session.PreviewStarter(_starterDrafts[_starterPreviewIndex]); _starterFeedback.Text="Private preview updated."; RefreshStarterChrome();
    }

    private void ApplyValidStarterBranding()
    {
        var color=_starterBrandAccent.Text.Trim();
        if(!_loading && color.Length==7 && color[0]=='#' && color.Skip(1).All(Uri.IsHexDigit)) ApplyStarterBranding();
    }

    private async Task PickStarterLogoAsync()
    {
        if (_session is null || _activeStarter is null || _starterAdding || _starterLogoImporting) return;
        _starterLogoImporting=true; RefreshStarterChrome();
        var starter=_activeStarter; var epoch=_session.State?.CoreEpoch;
        try {
            var picker=new Windows.Storage.Pickers.FileOpenPicker(); foreach(var extension in new[] {".png",".jpg",".jpeg"}) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker,App.MainWindowHandle);var file=await picker.PickSingleFileAsync();if(file is null)return;
            var image=await _session.ImportImageAsync(file.Path);
            if(image is null) {_starterFeedback.Text=_session.ImportStatus;return;}
            if(_activeStarter!=starter || _session.State?.CoreEpoch!=epoch)return;
            var template=_session.State?.Templates.FirstOrDefault(t=>t.TemplateId==image.TemplateId && t.TemplateVersion==image.TemplateVersion);
            if(template is null) {_starterFeedback.Text="The logo package is unavailable. Your samples were kept.";return;}
            RememberStarterEdits();
            _starterDrafts=GraphicsShowStarters.WithLogo(_starterDrafts,template,image);
            _session.PreviewStarter(_starterDrafts[_starterPreviewIndex]); _starterFeedback.Text="Your logo is applied to the private sample set. Program is unchanged.";
        } catch(Exception ex) when(ex is not OutOfMemoryException) {_starterFeedback.Text="Logo import failed: "+ex.Message;}
        finally {_starterLogoImporting=false;RefreshStarterChrome();}
    }
}
