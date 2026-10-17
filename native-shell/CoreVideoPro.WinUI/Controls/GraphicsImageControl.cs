using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;
public sealed class GraphicsImageControl : UserControl
{
    private readonly GraphicsWorkspaceSession _session;
    private readonly ComboBox _image=new() {Header="Logo image · verified package",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly CheckBox _show=new() {Content="Show logo"};
    private readonly Button _import=new() {Content="Import local PNG / JPEG…"};
    private readonly TextBlock _status=new() {TextWrapping=TextWrapping.Wrap,FontSize=11};
    private readonly List<NativeGraphicsImageReference?> _references=[];
    private bool _loading;
    private string _signature="";
    public GraphicsImageControl(GraphicsWorkspaceSession session)
    {
        _session=session;var panel=new StackPanel {Spacing=8};panel.Children.Add(_image);panel.Children.Add(_show);panel.Children.Add(_import);panel.Children.Add(_status);Content=panel;
        _image.SelectionChanged+=(_,_)=>{if(!_loading && _image.SelectedIndex>=0 && _image.SelectedIndex<_references.Count && _session.Draft is { } draft) _session.Edit(draft with {Image=_references[_image.SelectedIndex]});};
        _show.Checked+=(_,_)=>SetVisible(true);_show.Unchecked+=(_,_)=>SetVisible(false);_import.Click+=async(_,_)=>await PickAsync();Refresh();
    }
    private void SetVisible(bool visible) {if(!_loading && _session.Draft is { } draft) _session.SetStyle(draft.Style with {ShowLogo=visible});}
    public void Refresh()
    {
        _loading=true;
        try {
            var templates=_session.State?.Templates ?? [];
            var signature=string.Join('|',templates.Select(t=>t.ManifestHash));
            if(signature!=_signature || _image.Items.Count==0) {
                _signature=signature;_references.Clear();_image.Items.Clear();_references.Add(null);_image.Items.Add("No image");
                foreach(var template in templates) if(template.Definition is { } definition && definition.TryGetProperty("assets",out var assets))
                    foreach(var asset in assets.EnumerateArray()) {var id=asset.GetProperty("id").GetString()!;_references.Add(new(template.TemplateId,template.TemplateVersion,id));_image.Items.Add($"{template.Name} v{template.TemplateVersion} · {id}");}
            }
            _image.SelectedIndex=_references.FindIndex(r=>r==_session.Draft?.Image);
            _show.IsChecked=_session.Draft?.Style.ShowLogo;
            _image.IsEnabled=_show.IsEnabled=!_session.Busy;
            _import.IsEnabled=!_session.ImportBusy;
            _status.Text=_session.ImportBusy?_session.ImportStatus:_image.SelectedIndex<0?"Saved logo package is unavailable. Import its pinned version; the reference is retained.":"Logo and visibility changes remain private until Cue or Update. Native image decoding preserves aspect ratio and alpha.";
        } finally {_loading=false;}
    }
    private async Task PickAsync()
    {
        var before=_session.Draft;var selectedId=_session.SelectedId;
        try {
            var picker=new Windows.Storage.Pickers.FileOpenPicker();foreach(var extension in new[] {".png",".jpg",".jpeg"}) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker,App.MainWindowHandle);var file=await picker.PickSingleFileAsync();if(file is null) return;
            var image=await _session.ImportImageAsync(file.Path);
            if(image is not null && _session.SelectedId==selectedId && _session.Draft is { } current && current.TemplateId==before?.TemplateId && current.Image==before.Image)
                _session.Edit(current with {Image=image});
            Refresh();_status.Text=_session.ImportStatus;
        } catch(Exception ex) when(ex is not OutOfMemoryException) {_status.Text="Image import unavailable: "+ex.Message;}
    }
}
