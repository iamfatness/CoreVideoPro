using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsWorkspaceControl
{
    private void BuildLibraryTools()
    {
        _libraryPanel.Children.Add(new TextBlock { Text = "LIBRARY", FontSize = 11, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        _libraryPanel.Children.Add(_search); _libraryPanel.Children.Add(_family);_libraryPanel.Children.Add(_libraryView);_libraryPanel.Children.Add(Action("Refresh library",async()=>{if(_session is not null) await _session.RefreshCatalogAsync();})); _libraryPanel.Children.Add(_library);
        _libraryPanel.Children.Add(_scope);
        _libraryPanel.Children.Add(Action("Create hidden cue", async () => { if (_library.SelectedIndex >= 0 && _session is not null && _scope.SelectedItem is ComboBoxItem {Tag:string scene}) await _session.CreateAsync(_session.Selected is null && _session.Template is { } draftTemplate ? draftTemplate : _templateRows[_library.SelectedIndex],scene.Length==0?"global":"scene",scene); }));
        _libraryPanel.Children.Add(new TextBlock { Text = "SAVED CUES", FontSize = 11, Margin = new Thickness(0, 12, 0, 0) });
        _libraryPanel.Children.Add(_savedCues);
        _libraryPanel.Children.Add(Action("Restore as hidden cue", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.RestoreAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex],_session?.Draft?.Binding); }));
        var savedActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        savedActions.Children.Add(Action("Move up", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.MoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id, -1); }));
        savedActions.Children.Add(Action("Move down", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.MoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id, 1); }));
        _libraryPanel.Children.Add(savedActions);
        _libraryPanel.Children.Add(Action("Delete saved copy", async () => { if (_savedLibrary is not null && _savedCues.SelectedIndex >= 0) await _savedLibrary.RemoveSavedAsync(_savedLibrary.Document.Cues[_savedCues.SelectedIndex].Id); }));
        _libraryPanel.Children.Add(_saveStatus);
        var migration=new StackPanel {Spacing=8};
        migration.Children.Add(Copy("Showing a Volt lower third selects its instance authority and suppresses the legacy family. Hide/Clear does not reveal the old key. Existing lower-third API and key controls address the managed cues."));
        migration.Children.Add(Action("Copy existing look and overrides",async()=>{if(Studio is not null) await Studio.CopyLegacyGraphicsToLibraryAsync();}));
        migration.Children.Add(Action("Use legacy lower-thirds",async()=>{if(_session is not null && _savedLibrary is not null && await _savedLibrary.ClearStudioLowerThirdAsync()) await _session.SetAuthorityAsync("lower-third","legacy");}));
        migration.Children.Add(Copy("Returning to legacy explicitly clears managed lower thirds. Captions and unrelated overlays keep their owners. Saved source-bound cues require choosing their current subject in the inspector before Restore."));
        _libraryPanel.Children.Add(new Expander {Header="Existing show compatibility",Content=migration,HorizontalAlignment=HorizontalAlignment.Stretch});
        var import = new TextBox { Header = "Import declarative template", PlaceholderText = "Paste template.json", AcceptsReturn = true, Height = 110, MaxLength = 262144 };
        var importPanel = new StackPanel { Spacing = 8 }; importPanel.Children.Add(import);
        importPanel.Children.Add(Copy("Local declarations only. Validation is asynchronous; the previous version and on-air graphics remain intact."));
        importPanel.Children.Add(Action("Validate and import", async () => { if (_session is not null) await _session.ImportAsync(import.Text); }));
        _libraryPanel.Children.Add(new Expander { Header = "Import template", Content = importPanel, HorizontalAlignment = HorizontalAlignment.Stretch });
        _packageImportButton = Action("Import graphics package…", PickPackageAsync); _libraryPanel.Children.Add(_packageImportButton); _libraryPanel.Children.Add(_importStatus);
        _libraryPanel.Children.Add(Action("Export selected template package…",ExportPackageAsync));
    }
    private void BuildCueManagement()
    {
        _libraryPanel.Children.Add(new TextBlock { Text = "CUE RACK", FontSize = 11, Margin = new Thickness(0, 12, 0, 0) });
        _deleteButton = Action("Delete hidden cue", async () => { if (_session is not null) await _session.RemoveAsync(); }); _libraryPanel.Children.Add(_deleteButton);
        _libraryPanel.Children.Add(Action("Duplicate into hidden cue", async () => { if (_session is not null) await _session.DuplicateAsync(); }));
        _libraryPanel.Children.Add(_saveName);
        _libraryPanel.Children.Add(Action("Rename selected cue",async()=>{if(_session is not null) await _session.RenameCueAsync(_saveName.Text);}));
        _libraryPanel.Children.Add(Action("Move cue earlier / behind",async()=>{if(_session is not null) await _session.MoveCueAsync(-1);}));
        _libraryPanel.Children.Add(Action("Move cue later / in front",async()=>{if(_session is not null) await _session.MoveCueAsync(1);}));
        _libraryPanel.Children.Add(Action("Save cue", async () => { if (Studio is not null) await Studio.SaveGraphicsCueAsync(_saveName.Text); }));
    }
}
