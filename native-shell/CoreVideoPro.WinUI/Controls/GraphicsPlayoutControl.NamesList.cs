using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsPlayoutControl
{
    private DropDownButton NamesListEditor(GraphicsPlaylistRow row, IReadOnlyCollection<string> variableIds)
    {
        var text = new TextBox
        {
            Header = "Names list CSV", PlaceholderText = "name,title,location",
            AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 130, MaxHeight = 260
        };
        var status = new TextBlock { TextWrapping = TextWrapping.Wrap };
        var save = new Button { Content = "Save names list" };
        var import = new Button { Content = "Import CSV…" };
        var content = new StackPanel { Width = 320, Spacing = 8 };
        content.Children.Add(text); content.Children.Add(import); content.Children.Add(save); content.Children.Add(status);
        var flyout = new Flyout { Content = content };
        flyout.Opening += (_, _) =>
        {
            text.Text = GraphicsBuilder.NamesListText(_attached?.GraphicsPlayout.Row(row.Id)?.Variants ?? []);
            status.Text = "";
        };
        import.Click += async (_, _) =>
        {
            try
            {
                var picker = new Windows.Storage.Pickers.FileOpenPicker();
                picker.FileTypeFilter.Add(".csv");
                WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
                if (await picker.PickSingleFileAsync() is { } file)
                    text.Text = await System.IO.File.ReadAllTextAsync(file.Path);
            }
            catch (Exception ex) when (ex is not OutOfMemoryException) { status.Text = ex.Message; }
        };
        save.Click += async (_, _) =>
        {
            if (_attached is not { } studio || studio.GraphicsPlayout.Row(row.Id) is not { } current ||
                current.TemplateId != row.TemplateId || current.TemplateVersion != row.TemplateVersion) return;
            var variants = GraphicsBuilder.ParseNamesList(text.Text, current.Variants, variableIds);
            if (variants.Count == 0 && !string.IsNullOrWhiteSpace(text.Text))
            {
                status.Text = "No names found. Use Name, Title and optional variable IDs in the header.";
                return;
            }
            save.IsEnabled = false;
            try
            {
                                var updated = current with
                {
                    Variants = variants,
                    Kind = variants.Count > 0 ? GraphicsPlaylistRow.Title : current.FollowSources ? GraphicsPlaylistRow.Guest : current.Kind
                };
                var rows = GraphicsBuilder.Upsert(studio.GraphicsPlayout.Playlist, updated);
                if (await studio.GraphicsLibrary.SavePlaylistAsync(rows)) flyout.Hide();
                else status.Text = studio.GraphicsLibrary.Status;
            }
            catch (Exception ex) when (ex is not OutOfMemoryException) { status.Text = ex.Message; }
            finally { save.IsEnabled = true; }
        };
        return new DropDownButton { Content = "Names list…", Flyout = flyout, HorizontalAlignment = HorizontalAlignment.Stretch };
    }
}
