using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage
{
    private sealed record CatalogChoice(string Value,string Label,string Description)
    {public override string ToString()=>Label+"\n"+Description;}
    private async void OnAddSourceCatalog(object sender,RoutedEventArgs e)
    {
        if(ViewModel is not {} studio||Workspace is not {} workspace)return;
        var sceneId=studio.PreviewSceneId;
        var search=new AutoSuggestBox{PlaceholderText="Search source types",Margin=new(0,0,0,8)};
        var list=new ListView{MaxHeight=340,MinHeight=220,SelectionMode=ListViewSelectionMode.Single};
        list.ItemTemplate=(DataTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load("<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><StackPanel Spacing='4' Padding='8'><TextBlock Text='{Binding Label}' FontSize='13' FontWeight='SemiBold'/><TextBlock Text='{Binding Description}' FontSize='11' Foreground='#9AA5B4' TextWrapping='Wrap'/></StackPanel></DataTemplate>");
        var catalogWidth=Math.Min(500,Math.Max(240,XamlRoot.Size.Width-96));
        var columns=catalogWidth>=420?2:1;
        list.ItemsPanel=(ItemsPanelTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load($"<ItemsPanelTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><ItemsWrapGrid Orientation='Horizontal' MaximumRowsOrColumns='{columns}' ItemWidth='{(catalogWidth/columns).ToString(System.Globalization.CultureInfo.InvariantCulture)}' ItemHeight='86'/></ItemsPanelTemplate>");
        list.MaxHeight=Math.Max(120,Math.Min(340,XamlRoot.Size.Height-280));list.MinHeight=Math.Min(220,list.MaxHeight);
        var help=new TextBlock{Text="Choose a type, then reuse a feed or create an authored source.",TextWrapping=TextWrapping.Wrap,FontSize=12};
        var content=new StackPanel{Spacing=10,MinWidth=catalogWidth};content.Children.Add(search);content.Children.Add(list);content.Children.Add(help);
        var dialog=new ContentDialog{XamlRoot=XamlRoot,Title="Add source",Content=content,PrimaryButtonText="Next",CloseButtonText="Cancel",IsPrimaryButtonEnabled=false};
        CatalogChoice[] types=[new("tiles","▦  CoreVideo Tiles","One auto-reflowing wall, mixed with your other layers."),new("camera","◉  Camera / Zoom guest","Reuse a guest, role, camera or show input."),new("share","▤  Screen share","Reuse the meeting’s shared screen or application."),new("image","▧  Image / logo","Reuse media or import an image."),new("text","T  Text","Create an editable title."),new("color","▰  Color background","Create a color layer.")];
        IReadOnlyList<CatalogChoice> choices=types;
        void Filter()=>list.ItemsSource=choices.Where(c=>(c.Label+" "+c.Description).Contains(search.Text??"",StringComparison.OrdinalIgnoreCase)).ToList();
        search.TextChanged+=(_,_)=>Filter();
        list.SelectionChanged+=(_,_)=>
        {
            dialog.IsPrimaryButtonEnabled=list.SelectedItem is CatalogChoice choice&&(choice.Value!="tiles"||workspace.AddTilesCommand.CanExecute(null));
            if(list.SelectedItem is CatalogChoice {Value:"tiles"}&&!workspace.AddTilesCommand.CanExecute(null))help.Text="This scene already has a Tiles wall. Select it in Sources to edit it.";
        };
        Filter();
        var choosingType=true;string? requested=null;
        dialog.PrimaryButtonClick+=(_,args)=>
        {
            if(list.SelectedItem is not CatalogChoice choice){args.Cancel=true;return;}
            if(!choosingType){requested=choice.Value;return;}
            if(choice.Value is "tiles" or "text" or "color"){requested=choice.Value;return;}
            choosingType=false;args.Cancel=true;dialog.Title="Add "+choice.Label[3..];dialog.PrimaryButtonText="Add source";
            list.ItemsPanel=(ItemsPanelTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load("<ItemsPanelTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><ItemsStackPanel/></ItemsPanelTemplate>");
            choices=choice.Value switch
            {
                "image"=>studio.OverlayMediaAssets.Where(a=>!Path.GetFileName(a.FilePath).StartsWith("scene-graphic-",StringComparison.OrdinalIgnoreCase)).Select(a=>new CatalogChoice("media:"+a.Id,a.Name,"Reuse existing image/media")).Prepend(new("import-image","Import image…","Create a media feed from a file.")).ToList(),
                "share"=>studio.CaptureDevices.Where(d=>d.Id.StartsWith("screen:",StringComparison.Ordinal)||d.Id.StartsWith("window:",StringComparison.Ordinal)).Select(d=>new CatalogChoice("capture:"+d.Id,d.Name,"Reuse existing screen capture")).Prepend(new("screen-share","Meeting screen share","Follow the shared screen in this meeting.")).ToList(),
                _=>studio.AddSourceOptions.Where(o=>o.Value is not ("media" or "screen-share")).Select(o=>new CatalogChoice(o.Value,o.Label,"Reuse show input, role or live mode"))
                    .Concat(studio.CaptureDevices.Select(d=>new CatalogChoice("capture:"+d.Id,d.Name,"Reuse existing capture")))
                    .Concat(studio.RoomVideoParticipants.Select(p=>new CatalogChoice("zoom:"+p.Id,p.Name,"Reuse meeting participant"))).ToList()
            };
            search.Text="";search.PlaceholderText="Search available sources";help.Text="Adds an instance to this scene. Reusing a feed does not start another capture session.";Filter();dialog.IsPrimaryButtonEnabled=false;
        };
        if(await dialog.ShowAsync()!=ContentDialogResult.Primary||requested is null||studio.PreviewSceneId!=sceneId||Workspace!=workspace)return;
        try
        {
        if(requested=="import-image")
        {workspace.Checkpoint();var imported=await studio.WorkspaceImportImageAsync(sceneId);if(imported is not null&&Workspace==workspace)workspace.Selected=workspace.Items.FirstOrDefault(i=>i.ItemId==imported.Id);return;}
        if(requested=="tiles")workspace.AddTilesCommand.Execute(null);
        else if(requested is "text" or "color")workspace.AddGraphic(requested);
        else {workspace.Checkpoint();var route=studio.WorkspaceAddFeed(requested);workspace.Selected=workspace.Items.FirstOrDefault(i=>i.ItemId==route.Id);}
        }
        catch(Exception ex) when(ex is ArgumentException or IOException or UnauthorizedAccessException or System.Runtime.InteropServices.ExternalException)
        {studio.CommandStatus="Could not add the source: "+ex.Message;}
    }
}
