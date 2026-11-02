using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.UI;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Template document controls. Preview pixels and validity come from
/// native authoring; edits do not install versions or change an accepted cue.</summary>
public sealed class GraphicsTemplateLayerEditor : UserControl
{
    private readonly StackPanel _root=new() { Spacing=8 },_properties=new() { Spacing=8 };
    private readonly ListView _layers=new() { MaxHeight=220,SelectionMode=ListViewSelectionMode.Single };
    private readonly TextBlock _status=new() { TextWrapping=TextWrapping.Wrap };
    private readonly List<Action> _refresh=[];
    private readonly List<Button> _layerCommands=[];
    private readonly Button _save=new() {Content="Save template as new version"};
    private CheckBox? _lockControl;
    private GraphicsWorkspaceSession? _session;
    private IReadOnlyList<GraphicsEditorLayer> _rows=[];
    private string _selected="",_structure="",_propertyKey="";
    private JsonElement _node; private bool _loading,_editing;
    public GraphicsWorkspaceSession? Session {
        get=>_session;
        set { if(_session is not null) {_session.EndGesture();_session.Changed-=Changed;} _session=value; if(_session is not null) _session.Changed+=Changed; _selected=_structure=_propertyKey=""; Refresh(); }
    }
    private readonly TextBox _templateName=new() {Header="Template name",MaxLength=128};
    /// <summary>True when the draft is one of the show's designs: Save then goes through the design
    /// lifecycle (<see cref="DesignSaveRequested"/>) instead of a bare template version.</summary>
    public Func<NativeGraphicsDraft,bool>? IsDesignDraft { get; set; }
    public event EventHandler? DesignSaveRequested;
    private bool DesignDraft=>_session?.Draft is { } draft && IsDesignDraft?.Invoke(draft)==true;
    // Variables come from the edited manifest, so a variable added in the Variables panel can be bound before saving.
    private IReadOnlyList<NativeGraphicsTemplateField> DocumentFields {
        get { try { return _session?.TemplateDocument is { } document ? GraphicsTemplateEditor.TemplateFields(document) : _session?.Template?.Fields ?? []; } catch(JsonException) { return _session?.Template?.Fields ?? []; } }
    }
    public GraphicsTemplateLayerEditor()
    {
        _root.Children.Add(new TextBlock { Text="Template layers",FontSize=16 });
        _root.Children.Add(_templateName);
        _templateName.LostFocus+=(_,_)=>{if(!_loading && _session?.TemplateDocument is not null) Change(()=>_session.EditTemplate(d=>GraphicsTemplateEditor.Rename(d,_templateName.Text)));};
        _root.Children.Add(new TextBlock { Text="Edit privately. Save a template version, then Cue or Update explicitly.",TextWrapping=TextWrapping.Wrap,FontSize=12 });
        var add=new StackPanel {Orientation=Orientation.Horizontal,Spacing=6};
        var kind=new ComboBox {ItemsSource=new[] {"rect","text","image","group"},SelectedIndex=1};add.Children.Add(kind);
        add.Children.Add(Button("Add layer",()=>{if(kind.SelectedItem is string type) _session?.EditTemplate(d=>GraphicsTemplateEditor.Add(d,type));}));
        _root.Children.Add(add);_root.Children.Add(_layers);
        var commands=new StackPanel { Orientation=Orientation.Horizontal,Spacing=6 };
        commands.Children.Add(Button("Up",()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Move(d,_selected,1))));
        commands.Children.Add(Button("Down",()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Move(d,_selected,-1))));
        commands.Children.Add(Button("Duplicate",()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Duplicate(d,_selected))));
        commands.Children.Add(Button("Remove",()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Remove(d,_selected))));
        _layerCommands.AddRange(commands.Children.OfType<Button>());
        _root.Children.Add(commands); _root.Children.Add(_properties); _root.Children.Add(_status);
        _save.Click+=async(_,_)=>{if(DesignDraft) DesignSaveRequested?.Invoke(this,EventArgs.Empty); else if(_session is not null) await _session.SaveTemplateVersionAsync();};
        _root.Children.Add(_save); Content=_root;
        _layers.SelectionChanged+=(_,_)=>{ if(!_loading && _layers.SelectedIndex>=0 && _layers.SelectedIndex<_rows.Count) { _session?.EndGesture(); _selected=_rows[_layers.SelectedIndex].Id; _session?.SelectLayer(_selected); Refresh(); } };
    }
    private static Button Button(string title,Action action) {var button=new Button { Content=title };button.Click+=(_,_)=>action();return button;}
    private void Changed(object? sender,EventArgs e) {if(!_editing && !_loading) Refresh();}
    private void Change(Action action) { _editing=true;try {action();} finally {_editing=false;} Refresh(); }
    private void Refresh()
    {
        if(_session?.Draft?.Declarative!=true || _session.TemplateDocument is not { } document) {Visibility=Visibility.Collapsed;return;}
        Visibility=Visibility.Visible; _loading=true;
        try {
            if(_templateName.FocusState==FocusState.Unfocused) {using var template=JsonDocument.Parse(document.Manifest);_templateName.Text=template.RootElement.GetProperty("name").GetString() ?? "";}
            var authored=GraphicsTemplateEditor.Layers(document);
            IEnumerable<GraphicsEditorLayer> FrontToBack(string parent) {
                foreach(var layer in authored.Where(l=>l.Parent==parent).Reverse()) {yield return layer;foreach(var child in FrontToBack(layer.Id)) yield return child;}
            }
            _rows=FrontToBack("").ToArray();
            var structure=string.Join('|',_rows.Select(r=>$"{r.Id}:{r.Label}:{r.Depth}:{r.Locked}:{r.Visible}"));
            if(structure!=_structure) {
                _structure=structure; _layers.Items.Clear();
                foreach(var row in _rows) _layers.Items.Add(new TextBlock { Text=new string(' ',row.Depth*2)+(row.Locked?"Locked · ":"")+(row.Visible?"":"Hidden · ")+row.Label+" · "+row.Type });
            }
            if(_rows.Any(r=>r.Id==_session.SelectedLayerId)) _selected=_session.SelectedLayerId;
            if(!_rows.Any(r=>r.Id==_selected)) _selected=_rows.FirstOrDefault()?.Id ?? "";
            if(_session.SelectedLayerId!=_selected) _session.SelectLayer(_selected);
            _layers.SelectedIndex=_rows.ToList().FindIndex(r=>r.Id==_selected);
            if(_selected.Length==0) return;
            _node=GraphicsTemplateEditor.Layer(document,_selected);
            var rowInfo=_rows.First(r=>r.Id==_selected);
            var key=_session.Draft.TemplateId+":"+_session.Draft.TemplateVersion+":"+_selected+":"+rowInfo.Type+":"+string.Join('|',_rows.Where(r=>r.Type=="group").Select(r=>r.Id))+":"+string.Join(',',DocumentFields.Select(f=>f.Id+"="+f.Type));
            if(key!=_propertyKey) { _propertyKey=key;BuildProperties(rowInfo); }
            foreach(var control in _properties.Children.OfType<Microsoft.UI.Xaml.Controls.Control>()) control.IsEnabled=!_session.Busy && (!rowInfo.Locked || ReferenceEquals(control,_lockControl));
            foreach(var button in _layerCommands) button.IsEnabled=!_session.Busy && !rowInfo.Locked;
            _save.IsEnabled=!_session.Busy && !_session.TemplateSaveBusy && _session.Draft.Scratch is not null;
            _save.Content=DesignDraft?"Save design":"Save template as new version";
            foreach(var refresh in _refresh) refresh();
            _status.Text=_session.TemplateSaveBusy?"Validating and saving template version. Live controls remain available.":_session.Draft.Scratch is not null?"Unsaved template edits. Accepted Cue and Program are unchanged.":"Using an installed immutable template version.";
        } catch(Exception ex) when(ex is InvalidDataException or InvalidOperationException or JsonException) {_status.Text=ex.Message;}
        finally {_loading=false;}
    }
    private void BuildProperties(GraphicsEditorLayer layer)
    {
        _properties.Children.Clear(); _refresh.Clear();
        var visible=new CheckBox { Content="Show layer" }; visible.Checked+=(_,_)=>Set("visible",true);visible.Unchecked+=(_,_)=>Set("visible",false);
        var locked=new CheckBox { Content="Lock layer" };_lockControl=locked; locked.Checked+=(_,_)=>Lock(true);locked.Unchecked+=(_,_)=>Lock(false);
        _properties.Children.Add(visible);_properties.Children.Add(locked);
        var parent=new ComboBox {Header="Parent group",ItemsSource=new[] {"Canvas root"}.Concat(_rows.Where(r=>r.Type=="group" && r.Id!=layer.Id).Select(r=>r.Id)).ToArray(),SelectedItem=layer.Parent.Length==0?"Canvas root":layer.Parent,HorizontalAlignment=HorizontalAlignment.Stretch};
        parent.SelectionChanged+=(_,_)=>{if(!_loading && parent.SelectedItem is string id) Change(()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Reparent(d,_selected,id=="Canvas root"?"":id)));};_properties.Children.Add(parent);
        _refresh.Add(()=>{var current=_rows.FirstOrDefault(r=>r.Id==_selected);parent.SelectedItem=string.IsNullOrEmpty(current?.Parent)?"Canvas root":current.Parent;});
        _refresh.Add(()=>{var row=_rows.FirstOrDefault(r=>r.Id==_selected);visible.IsChecked=row?.Visible==true;locked.IsChecked=row?.Locked==true;});
        foreach(var property in new[] {"x","y","width","height"}) Number(property,property is "width" or "height"?1:0,8192,1,"px",1);
        Number("opacity",0,1,.01,"%",100);
        if(layer.Type=="rect") {Number("cornerRadius",0,4096,1,"px",1);Fill();}
        if(layer.Type=="text") {
            TextContent();
            Number("fontSize",1,512,1,"px",1);NumericChoice("fontWeight",[(400,"Regular"),(500,"Medium"),(600,"Semibold"),(700,"Bold")]);Number("lineHeight",.5,3,.05,"×",1);NumericChoice("maxLines",[(1,"One line"),(2,"Two lines"),(3,"Three lines")]);
            Choice("align",["left","center","right"]);Choice("overflow",["ellipsis","wrap-then-ellipsis"]);Color("color");
            var source=new ComboBox {Header="Font source",SelectedValuePath="Tag",HorizontalAlignment=HorizontalAlignment.Stretch};
            foreach(var choice in new[] {("body","Brand body font"),("heading","Brand heading font"),("custom","Custom font")}) source.Items.Add(new ComboBoxItem {Content=choice.Item2,Tag=choice.Item1});
            var font=new TextBox {Header="Custom font family",MaxLength=128};
            source.SelectionChanged+=(_,_)=>{if(_loading) return;if(source.SelectedValue is "body" or "heading") Set("font",new {token=source.SelectedValue is "body"?"brand.fontBody":"brand.fontHeading"});else if(source.SelectedValue is "custom") Set("font",_session?.Draft?.Style.FontFamily ?? "Segoe UI");};
            font.TextChanged+=(_,_)=>{if(!_loading && font.FocusState!=FocusState.Unfocused && !string.IsNullOrWhiteSpace(font.Text)) Set("font",font.Text);};
            font.GotFocus+=(_,_)=>_session?.BeginGesture();font.LostFocus+=(_,_)=>_session?.EndGesture();
            _properties.Children.Add(source);_properties.Children.Add(font);
            _refresh.Add(()=>{var value=Read("font");var inherited=value.ValueKind==JsonValueKind.Object && value.TryGetProperty("token",out var token);
                source.SelectedValue=inherited && value.GetProperty("token").GetString()=="brand.fontHeading"?"heading":inherited?"body":"custom";
                font.IsEnabled=!inherited && _session?.Busy==false && _rows.FirstOrDefault(l=>l.Id==_selected)?.Locked!=true;if(font.FocusState==FocusState.Unfocused) font.Text=value.ValueKind==JsonValueKind.String?value.GetString() ?? "":_session?.Draft?.Style.FontFamily ?? "Segoe UI";});
        }
        if(layer.Type=="image") {ImageContent();Choice("fit",["contain","cover"]);}
        if(layer.Type=="group") {Number("padding",0,256,1,"px",1);Number("gap",0,256,1,"px",1);Choice("flow",["none","horizontal","vertical"]);}
    }
    private void TextContent()
    {
        var fields=DocumentFields.Where(f=>f.Type=="string").ToArray();
        var source=new ComboBox {Header="Text source",SelectedValuePath="Tag",HorizontalAlignment=HorizontalAlignment.Stretch};
        source.Items.Add(new ComboBoxItem {Content="Custom text",Tag="custom"});foreach(var field in fields) source.Items.Add(new ComboBoxItem {Content="Field: "+field.Id,Tag="field:"+field.Id});
        var text=new TextBox {Header="Text",AcceptsReturn=true,MaxLength=4096};
        source.SelectionChanged+=(_,_)=>{if(_loading) return;if(source.SelectedValue is string value && value.StartsWith("field:",StringComparison.Ordinal)) Set("text",new {field=value[6..]});else if(source.SelectedValue is "custom") Set("text",Read("text") is {ValueKind:JsonValueKind.String} old?old.GetString() ?? "":"Title");};
        text.TextChanged+=(_,_)=>{if(_loading || text.FocusState==FocusState.Unfocused) return;
            if(_node.TryGetProperty("text",out var value) && value.ValueKind==JsonValueKind.Object && value.TryGetProperty("field",out var field)) Change(()=>_session?.SetField(field.GetString()!,JsonSerializer.SerializeToElement(text.Text)));
            else Set("text",text.Text);};
        text.GotFocus+=(_,_)=>_session?.BeginGesture();text.LostFocus+=(_,_)=>_session?.EndGesture();
        _properties.Children.Add(source);_properties.Children.Add(text);
        _refresh.Add(()=>{source.SelectedValue=_node.TryGetProperty("text",out var raw) && raw.ValueKind==JsonValueKind.Object && raw.TryGetProperty("field",out var field)?"field:"+field.GetString():"custom";
            if(text.FocusState==FocusState.Unfocused) text.Text=Read("text") is {ValueKind:JsonValueKind.String} current?current.GetString() ?? "":"";});
    }
    private void ImageContent()
    {
        var box=new ComboBox {Header="Verified image source",SelectedValuePath="Tag",HorizontalAlignment=HorizontalAlignment.Stretch};
        if(_session?.Template?.Definition is { } definition && definition.TryGetProperty("assets",out var assets)) foreach(var asset in assets.EnumerateArray()) {
            var id=asset.GetProperty("id").GetString()!;box.Items.Add(new ComboBoxItem {Content="Asset: "+id,Tag="asset:"+id});
        }
        box.Items.Add(new ComboBoxItem {Content="Brand logo",Tag="token:brand.logo"});
        foreach(var field in DocumentFields.Where(f=>f.Type=="image")) box.Items.Add(new ComboBoxItem {Content="Field: "+field.Id,Tag="field:"+field.Id});
        box.SelectionChanged+=(_,_)=>{if(_loading || box.SelectedValue is not string value) return;if(value.StartsWith("field:",StringComparison.Ordinal)) Set("asset",new {field=value[6..]});else if(value.StartsWith("token:",StringComparison.Ordinal)) Set("asset",new {token=value[6..]});else Set("asset",value[6..]);};
        _properties.Children.Add(box);_refresh.Add(()=>{if(!_node.TryGetProperty("asset",out var value)) return;box.SelectedValue=value.ValueKind==JsonValueKind.Object && value.TryGetProperty("field",out var field)?"field:"+field.GetString():value.ValueKind==JsonValueKind.Object && value.TryGetProperty("token",out var token)?"token:"+token.GetString():value.ValueKind==JsonValueKind.String?"asset:"+value.GetString():null;});
    }
    private JsonElement Read(string property)
    {
        if(!_node.TryGetProperty(property,out var value)) return default;
        if(value.ValueKind==JsonValueKind.Object && value.TryGetProperty("field",out var field) && _session?.Draft?.Fields.TryGetProperty(field.GetString()!,out var bound)==true) return bound;
        return value;
    }
    private void Set<T>(string property,T value) {if(_loading) return;Change(()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Set(d,_selected,property,JsonSerializer.SerializeToElement(value))));}
    private void Lock(bool value) {if(_loading) return;Change(()=>_session?.EditTemplate(d=>GraphicsTemplateEditor.Lock(d,_selected,value)));}
    private void Number(string property,double min,double max,double step,string unit,double displayScale)
    {
        var value=Read(property);var neutral=property=="opacity"?1:property=="lineHeight"?1.2:property=="maxLines"?1:property=="fontWeight"?400:Math.Max(min,0);
        var slider=new SliderValueControl {Header=Label(property),Minimum=min,Maximum=max,StepFrequency=step,Unit=unit,DisplayScale=displayScale,DefaultValue=neutral,Value=value.ValueKind==JsonValueKind.Number?value.GetDouble():neutral};
        slider.ValueChanged+=(_,n)=>{if(!_loading) Change(()=>_session?.SetLayerNumber(_selected,property,n));};
        slider.EditStarted+=(_,_)=>_session?.BeginGesture();slider.EditCompleted+=(_,_)=>_session?.EndGesture();_properties.Children.Add(slider);
        _refresh.Add(()=>{var current=Read(property);slider.Value=current.ValueKind==JsonValueKind.Number?current.GetDouble():neutral;});
    }
    private void NumericChoice(string property,(double Value,string Name)[] choices)
    {
        var box=new ComboBox {Header=Label(property),SelectedValuePath="Tag",HorizontalAlignment=HorizontalAlignment.Stretch};
        foreach(var choice in choices) box.Items.Add(new ComboBoxItem {Content=choice.Name,Tag=choice.Value});
        box.SelectionChanged+=(_,_)=>{if(!_loading && box.SelectedValue is double n) Change(()=>_session?.SetLayerNumber(_selected,property,n));};
        _properties.Children.Add(box);_refresh.Add(()=>{var value=Read(property);box.SelectedValue=value.ValueKind==JsonValueKind.Number?value.GetDouble():choices[0].Value;});
    }
    private static string Label(string property)=>property switch {"x"=>"Horizontal position","y"=>"Vertical position","width"=>"Layer width","height"=>"Layer height","fontSize"=>"Font size","fontWeight"=>"Text weight","lineHeight"=>"Line spacing","maxLines"=>"Maximum lines","cornerRadius"=>"Corner radius","overflow"=>"Text overflow","align"=>"Alignment","fill"=>"Fill colour","color"=>"Text colour",_=>char.ToUpperInvariant(property[0])+property[1..]};
    private void Choice(string property,string[] choices)
    {
        var box=new ComboBox {Header=Label(property),ItemsSource=choices,HorizontalAlignment=HorizontalAlignment.Stretch};
        box.SelectionChanged+=(_,_)=>{if(!_loading && box.SelectedItem is string value) Set(property,value);};_properties.Children.Add(box);
        _refresh.Add(()=>{var value=Read(property);box.SelectedItem=value.ValueKind==JsonValueKind.String?value.GetString():choices[0];});
    }
    private string ResolveColor(JsonElement value)
    {
        if(value.ValueKind==JsonValueKind.String) return value.GetString() ?? "#FFFFFF";
        if(value.ValueKind!=JsonValueKind.Object) return "#FFFFFF";
        if(value.TryGetProperty("field",out var field) && _session?.Draft?.Fields.TryGetProperty(field.GetString()!,out var current)==true) return ResolveColor(current);
        if(value.TryGetProperty("stops",out var stops) && stops.ValueKind==JsonValueKind.Array && stops.GetArrayLength()==2) return ResolveColor(stops[0]);
        if(value.TryGetProperty("token",out var token) && _session?.Draft?.Style is { } style)
            return token.GetString() switch {"brand.primary"=>style.BackgroundColor+((byte)Math.Round(style.BackgroundOpacity*255,MidpointRounding.AwayFromZero)).ToString("X2"),"brand.background"=>style.BackgroundColor,"brand.accent"=>style.AccentColor,"brand.onPrimaryMuted" or "brand.secondary"=>style.TitleColor,_=>style.NameColor};
        return "#FFFFFF";
    }
    private string ColorText(string property)=>ResolveColor(Read(property));
    private bool Gradient=>_node.TryGetProperty("fill",out var fill) && fill.ValueKind==JsonValueKind.Object && fill.TryGetProperty("stops",out _);
    private void Fill()
    {
        var panel=new StackPanel {Spacing=8};var mode=new ComboBox {Header="Fill mode",SelectedValuePath="Tag",HorizontalAlignment=HorizontalAlignment.Stretch};
        foreach(var choice in new[] {("solid","Solid colour"),("gradient","Two-colour gradient"),("brand.primary","Brand plate"),("brand.accent","Brand accent"),("brand.background","Brand background"),("brand.foreground","Brand foreground"),("brand.secondary","Brand secondary"),("brand.onPrimary","Brand on-plate text"),("brand.onPrimaryMuted","Brand on-plate secondary")}) mode.Items.Add(new ComboBoxItem {Tag=choice.Item1,Content=choice.Item2});
        var start=new ColorPicker {IsAlphaEnabled=true};var end=new ColorPicker {IsAlphaEnabled=true};
        var direction=new ComboBox {Header="Gradient direction",ItemsSource=new[] {"horizontal","vertical"},HorizontalAlignment=HorizontalAlignment.Stretch};
        void EditGradient(Action<JsonObject> update) {
            if(_loading || !Gradient) return;var fill=JsonNode.Parse(_node.GetProperty("fill").GetRawText())!.AsObject();update(fill);Set("fill",JsonSerializer.SerializeToElement(fill));
        }
        mode.SelectionChanged+=(_,_)=>{if(_loading || mode.SelectedValue is not string selected) return;
            if(selected=="gradient") Set("fill",new {stops=new[] {ColorText("fill"),_session?.Draft?.Style.AccentColor ?? "#44C1A1"},direction="horizontal"});
            else if(selected=="solid") Set("fill",ColorText("fill"));else Set("fill",new {token=selected});};
        static string Rgba(Windows.UI.Color c)=>$"#{c.R:X2}{c.G:X2}{c.B:X2}{c.A:X2}";
        start.ColorChanged+=(_,e)=>{if(_loading) return;if(Gradient) EditGradient(fill=>fill["stops"]!.AsArray()[0]=JsonValue.Create(Rgba(e.NewColor)));else Set("fill",Rgba(e.NewColor));};
        end.ColorChanged+=(_,e)=>EditGradient(fill=>fill["stops"]!.AsArray()[1]=JsonValue.Create(Rgba(e.NewColor)));
        direction.SelectionChanged+=(_,_)=>{if(direction.SelectedItem is string value) EditGradient(fill=>fill["direction"]=value);};
        panel.Children.Add(mode);panel.Children.Add(start);panel.Children.Add(end);panel.Children.Add(direction);
        _properties.Children.Add(new Expander {Header="Fill and gradient",Content=panel,HorizontalAlignment=HorizontalAlignment.Stretch});
        _refresh.Add(()=>{
            mode.SelectedValue=Gradient?"gradient":_node.TryGetProperty("fill",out var fill) && fill.ValueKind==JsonValueKind.Object && fill.TryGetProperty("token",out var token)?token.GetString():"solid";
            start.Color=GraphicsColor.Parse(ColorText("fill"),start.Color);end.Visibility=direction.Visibility=Gradient?Visibility.Visible:Visibility.Collapsed;
            if(Gradient) {var value=_node.GetProperty("fill");end.Color=GraphicsColor.Parse(ResolveColor(value.GetProperty("stops")[1]),end.Color);direction.SelectedItem=value.TryGetProperty("direction",out var d)?d.GetString():"horizontal";}
        });
    }
    private void Color(string property)
    {
        var picker=new ColorPicker {IsAlphaEnabled=true};picker.ColorChanged+=(_,e)=>{if(!_loading) Set(property,$"#{e.NewColor.R:X2}{e.NewColor.G:X2}{e.NewColor.B:X2}{e.NewColor.A:X2}");};
        _properties.Children.Add(new Expander {Header=Label(property),Content=picker,HorizontalAlignment=HorizontalAlignment.Stretch});
        _refresh.Add(()=>{picker.Color=GraphicsColor.Parse(ColorText(property),picker.Color);});
    }
}
