using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace CoreVideoPro.WinUI.Controls;
public sealed class GraphicsTemplateCard : UserControl
{
    private readonly Image _image=new() {MaxWidth=208,Height=90,Stretch=Stretch.Uniform};
    private readonly TextBlock _status=new() {TextWrapping=TextWrapping.Wrap,FontSize=11};
    private readonly NativeGraphicsTemplate _template;
    private readonly bool _current;
    private bool _decoded;
    /// <param name="current">False while the core is still re-rendering this card in a new show palette
    /// (graphics styles slice 11): the old image stays, dimmed, labelled "Recolouring", never passed off as current.</param>
    public GraphicsTemplateCard(NativeGraphicsTemplate template,bool compact,bool current=true)
    {
        _template=template;_current=current;
        var panel=new StackPanel {Spacing=5,MaxWidth=215};
        panel.Children.Add(new TextBlock {Text=template.Name,TextWrapping=TextWrapping.Wrap,FontWeight=Microsoft.UI.Text.FontWeights.SemiBold});
        panel.Children.Add(new TextBlock {Text=$"{template.Family} · v{template.TemplateVersion}",FontSize=11});
        if(!compact) {
            panel.Children.Add(new Border {Background=new SolidColorBrush(Windows.UI.Color.FromArgb(255,26,30,37)),Child=_image,Padding=new Thickness(4)});
            _status.Text=!current?(template.Thumbnail is {State:"ready"}?"Recolouring in your show palette…":"Native thumbnail preparing in your show palette…"):template.Thumbnail is {State:"ready",PaletteRevision:not null}?"Native render · your show palette":
                template.Thumbnail is {State:"ready"}?"Native render · reference appearance":template.Thumbnail?.Reason is {Length:>0} reason?reason:"Native thumbnail preparing…";
            if(!current) _image.Opacity=.45;
            panel.Children.Add(_status);
            var tokens=template.BrandTokens.Count==0?"Compatibility look":string.Join(", ",template.BrandTokens);
            ToolTipService.SetToolTip(panel,"Brand roles: "+tokens+". Open the private canvas for your current style, text and asset diagnostics.");
            Loaded+=async(_,_)=>await DecodeAsync();
        }
        Content=panel;
    }
    private async Task DecodeAsync()
    {
        if(_decoded || _template.Thumbnail?.State!="ready") return;_decoded=true;
        var bytes=GraphicsThumbnailBytes.Decode(_template.Thumbnail);if(bytes is null) {_status.Text="Native thumbnail is invalid; open the private preview.";return;}
        try {
            using var stream=new InMemoryRandomAccessStream();using(var writer=new DataWriter(stream.GetOutputStreamAt(0))) {writer.WriteBytes(bytes);await writer.StoreAsync();}
            stream.Seek(0);var bitmap=new BitmapImage();await bitmap.SetSourceAsync(stream);_image.Source=bitmap;
            if(_template.Thumbnail.Reason.Length>0 && _current) _status.Text=_template.Thumbnail.Reason;
        } catch(Exception ex) when(ex is not OutOfMemoryException) {_status.Text="Thumbnail unavailable; use the native private preview.";}
    }
}
