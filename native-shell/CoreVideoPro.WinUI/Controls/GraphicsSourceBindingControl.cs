using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Selection edits a private draft. A live generation, rather than a
/// display name or recycled participant number, qualifies the explicit commit.</summary>
public sealed class GraphicsSourceBindingControl : UserControl
{
    private readonly ComboBox _source = new() { Header="Subject binding", HorizontalAlignment=HorizontalAlignment.Stretch };
    private readonly TextBlock _status = new() { TextWrapping=TextWrapping.Wrap };
    private readonly Button _copy = new() { Content="Use source name and secondary line" };
    private readonly GraphicsWorkspaceSession _session;
    private readonly StudioViewModel _studio;
    private NativeGraphicsSource[] _sources = [];
    private string _signature="";
    private bool _loading;
    public GraphicsSourceBindingControl(GraphicsWorkspaceSession session, StudioViewModel studio)
    {
        _session=session; _studio=studio;
        var panel=new StackPanel {Spacing=8};panel.Children.Add(_source);panel.Children.Add(_copy);panel.Children.Add(_status);Content=panel;
        _source.SelectionChanged+=(_,_)=>{if(!_loading && _source.SelectedIndex>=0) _session.SetBinding(_source.SelectedIndex==0?null:_sources[_source.SelectedIndex-1].Identity);};
        _copy.Click+=(_,_)=>{var selected=_sources.FirstOrDefault(s=>s.Identity==_session.Draft?.Binding);if(selected?.Identity is not { } identity) return;var text=_studio.GraphicsSourceText(identity.SourceId,selected.DisplayName);_session.CopySourceText(text.Name,text.Title);};
        Refresh();
    }
    public void Refresh()
    {
        _loading=true;
        try {
            var sources=(_session.State?.Sources ?? []).Where(s=>s.VideoOn && s.Identity is not null).ToArray();
            var signature=string.Join('|',sources.Select(s=>$"{s.Identity}:{s.DisplayName}"));
            if(signature!=_signature || _source.Items.Count==0) {
                _signature=signature;_sources=sources;_source.Items.Clear();_source.Items.Add("Unbound — show in selected scope");
                foreach(var source in sources) _source.Items.Add(source.DisplayName+" · "+source.Identity!.SourceId);
            }
            var binding=_session.Draft?.Binding;
            _source.SelectedIndex=binding is null?0:Array.FindIndex(_sources,s=>s.Identity==binding)+1;
            if(binding is not null && _source.SelectedIndex==0) _source.SelectedIndex=-1;
            _source.IsEnabled=!_session.Busy && _session.Draft is not null;
            _copy.IsEnabled=_source.IsEnabled && binding is not null && _source.SelectedIndex>0;
            _status.Text=binding is null?"Unbound graphic. Choosing a subject changes only this draft. Cue or Update applies it.":
                _source.SelectedIndex<0?"The selected subject generation is unavailable. Choose its current source explicitly; no automatic rebind.":
                "Bound graphics draw only when this exact subject is rendered. Source text respects existing overrides and intentional blank secondary lines; edits remain private.";
        } finally {_loading=false;}
    }
}
