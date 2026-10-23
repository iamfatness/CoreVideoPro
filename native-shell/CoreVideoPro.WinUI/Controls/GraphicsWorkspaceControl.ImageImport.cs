using Microsoft.UI.Xaml;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class GraphicsWorkspaceControl
{
    private async Task PickGraphicImageAsync()
    {
        var session = _session; if (session is null || session.ImportBusy) return;
        try {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            foreach (var extension in new[] { ".png", ".jpg", ".jpeg" }) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, App.MainWindowHandle);
            var file = await picker.PickSingleFileAsync(); if (file is null) return;
            var selected = session.SelectedId; var draft = session.Draft;
            var image = await session.ImportImageAsync(file.Path, fullCanvas: true);
            if (image is null) { _importStatus.Text = session.ImportStatus; return; }
            var template = session.State?.Templates.FirstOrDefault(t => t.TemplateId == image.TemplateId && t.TemplateVersion == image.TemplateVersion);
            if (template is null) { _importStatus.Text = "Imported graphic is unavailable. Program is unchanged."; return; }
            if (session.SelectedId != selected || session.Draft != draft) {
                _importStatus.Text = "Graphic imported into the library. Your newer selection or edits were kept."; return;
            }
            session.Browse(template);
            _family.SelectedIndex = 0;
            _search.Text = "";
            _importStatus.Text = "Graphic imported with transparency preserved. Create a hidden cue, then Cue and Show. Program is unchanged.";
        } catch (Exception ex) when (ex is not OutOfMemoryException) {
            _importStatus.Text = "Graphic import failed: " + ex.Message;
        }
    }
}
