using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed class LowerThirdDesignerControl : UserControl
{
    public static readonly DependencyProperty StudioProperty=DependencyProperty.Register(nameof(Studio),typeof(StudioViewModel),
        typeof(LowerThirdDesignerControl),new PropertyMetadata(null,(d,_)=>((LowerThirdDesignerControl)d).LoadDraft()));
    public StudioViewModel? Studio { get=>(StudioViewModel?)GetValue(StudioProperty); set=>SetValue(StudioProperty,value); }
    private LowerThirdAppearance _draft=new();
    private readonly Dictionary<string,SliderValueControl> _values=[];
    private readonly ComboBox _preset=new() { Header="Starter look",ItemsSource=new[] { "compact-solid","minimal-accent","broadcast" } };
    private readonly ComboBox _saved=new() { Header="Saved looks" };
    private readonly ComboBox _anchor=new() { Header="Anchor",ItemsSource=CaptionStyleHelper.LowerThirdPositionOptions };
    private readonly TextBox _font=new() { Header="Font family" },_nameColor=new() { Header="Name color (#RRGGBB)" },_titleColor=new() { Header="Title color (#RRGGBB)" },_presetName=new() { Header="Save look as" };
    private readonly TextBox _backgroundColor=new() { Header="Background color (#RRGGBB)" },_accentColor=new() { Header="Accent color (#RRGGBB)" };
    private readonly ColorPicker _backgroundPicker=new() { IsAlphaEnabled=false },_accentPicker=new() { IsAlphaEnabled=false };
    private readonly CheckBox _logo=new() { Content="Show source logo" };
    private readonly ToggleSwitch _compare=new() { Header="Compare applied look (preview only)" };
    private readonly ColorPicker _namePicker=new() { IsAlphaEnabled=false },_titlePicker=new() { IsAlphaEnabled=false };
    private readonly TextBlock _status=new() { Text="Draft — Apply look updates the live lower third.",TextWrapping=TextWrapping.Wrap,FontSize=11 };
    private readonly VideoSurfaceHost _preview=new() { Height=220 };
    private readonly TextBlock _previewStatus=new() { FontSize=11,TextWrapping=TextWrapping.Wrap };
    private ColorGradeEditorViewModel? _editor;
    private GradePreviewCoordinator? _coordinator;
    private bool _loading;
    internal void LoadReviewDraft(LowerThirdAppearance look) => SetDraft(look);
    private bool _previewActive;
    public void ActivatePreview() { _previewActive=true;StartPreview(); }
    public void DeactivatePreview() { _previewActive=false;_coordinator?.Dispose();_coordinator=null;_editor=null; }
    public LowerThirdDesignerControl()
    {
        var root=new StackPanel { Spacing=8 }; Content=root;
        root.Children.Add(new TextBlock { Text="Lower-third designer",FontSize=16 });
        root.Children.Add(_preview);root.Children.Add(_previewStatus);root.Children.Add(_compare);root.Children.Add(_status);root.Children.Add(_preset);root.Children.Add(_saved);root.Children.Add(_font);
        Add(root,"NameSize","Name size",16,120,42,1,"px",v=>_draft with { NameSize=v });
        Add(root,"TitleSize","Title size",12,80,28,1,"px",v=>_draft with { TitleSize=v });
        root.Children.Add(_nameColor);root.Children.Add(_titleColor);
        root.Children.Add(new Expander { Header="Choose name color",Content=_namePicker });root.Children.Add(new Expander { Header="Choose title color",Content=_titlePicker });
        root.Children.Add(_backgroundColor);root.Children.Add(new Expander {Header="Choose background color",Content=_backgroundPicker});
        root.Children.Add(_accentColor);root.Children.Add(new Expander {Header="Choose accent color",Content=_accentPicker});
        Add(root,"BackgroundOpacity","Background opacity",0,1,.9,.01,"%",v=>_draft with { BackgroundOpacity=v },100);
        Add(root,"Padding","Padding",0,64,20,1,"px",v=>_draft with { Padding=v });
        Add(root,"Width","Width",.15,.95,.6,.01,"%",v=>_draft with { Width=v },100);
        Add(root,"CornerRadius","Corners",0,60,8,1,"px",v=>_draft with { CornerRadius=v });
        root.Children.Add(_anchor);_anchor.SelectionChanged+=(_,_)=> {if (!_loading && _anchor.SelectedItem is string anchor) Edit(_draft with { Anchor=anchor });};
        Add(root,"SafeOffsetX","Safe offset from side",0,.25,.05,.005,"%",v=>_draft with { SafeOffsetX=v },100);
        Add(root,"SafeOffsetY","Safe offset from top / bottom",0,.25,.06,.005,"%",v=>_draft with { SafeOffsetY=v },100);
        root.Children.Add(_logo);
        Add(root,"LogoScale","Logo size",.5,2,1,.05,"×",v=>_draft with { LogoScale=v });
        root.Children.Add(new TextBlock { Text="Sizes reference a 1080-line canvas. Long names are trimmed; source text is edited in Sources.",TextWrapping=TextWrapping.Wrap,FontSize=11 });
        var actions=new StackPanel { Orientation=Orientation.Horizontal,Spacing=8 };
        var apply=new Button { Content="Apply look" };var cancel=new Button { Content="Cancel draft" };
        apply.Click+=(_,_)=> { if (!CommitText()) return; if (Studio?.Overlays.ApplyAppearance(_draft)==true) _status.Text="Look saved; live update requested. Source text is unchanged."; else _status.Text="Check font, colors and values before applying."; };
        cancel.Click+=(_,_)=>LoadDraft();actions.Children.Add(apply);actions.Children.Add(cancel);root.Children.Add(actions);
        var reset=new Button { Content="Reset draft to starter look" };reset.Click+=(_,_)=>SetDraft(LowerThirdAppearance.FromPreset(_draft.Preset));root.Children.Add(reset);
        root.Children.Add(_presetName);var save=new Button { Content="Save named look" };root.Children.Add(save);
        save.Click+=(_,_)=> { if (!CommitText()) return; if (Studio?.Overlays.SaveAppearancePreset(_presetName.Text,_draft)==true) { _saved.ItemsSource=Studio.Overlays.LowerThirdPresets.Keys.ToArray();_status.Text="Named look saved; Apply look sends it live."; } else _status.Text="Use a name of 1–80 characters and valid appearance settings."; };
        var duplicate=new Button { Content="Duplicate saved look" };root.Children.Add(duplicate);
        duplicate.Click+=(_,_)=> { if (_saved.SelectedItem is string name && Studio?.Overlays.LowerThirdPresets.TryGetValue(name,out var look)==true) {
            var stem=name.Length>68?name[..68]:name;
            var copy=stem+" copy";
            for (var suffix=2;Studio.Overlays.LowerThirdPresets.ContainsKey(copy);suffix++) copy=stem+" copy "+suffix;
            if(Studio.Overlays.SaveAppearancePreset(copy,look)) {_saved.ItemsSource=Studio.Overlays.LowerThirdPresets.Keys.ToArray();_saved.SelectedItem=copy;_presetName.Text=copy;}
        } };
        _compare.Toggled+=(_,_)=> { UpdatePreview();_status.Text=_compare.IsOn?"Previewing the applied look; draft retained.":"Previewing draft — Apply look sends it live."; };
        _namePicker.ColorChanged+=(_,e)=> {if (!_loading) {_nameColor.Text=HexColor.ToHex(e.NewColor);Edit(_draft with {NameColor=_nameColor.Text});}};
        _titlePicker.ColorChanged+=(_,e)=> {if (!_loading) {_titleColor.Text=HexColor.ToHex(e.NewColor);Edit(_draft with {TitleColor=_titleColor.Text});}};
        _backgroundPicker.ColorChanged+=(_,e)=> {if (!_loading) {_backgroundColor.Text=HexColor.ToHex(e.NewColor);Edit(_draft with {BackgroundColor=_backgroundColor.Text});}};
        _accentPicker.ColorChanged+=(_,e)=> {if (!_loading) {_accentColor.Text=HexColor.ToHex(e.NewColor);Edit(_draft with {AccentColor=_accentColor.Text});}};
        _preset.SelectionChanged+=(_,_)=> { if (!_loading && _preset.SelectedItem is string id) SetDraft(LowerThirdAppearance.FromPreset(id)); };
        _saved.SelectionChanged+=(_,_)=> { if (!_loading && _saved.SelectedItem is string name && Studio?.Overlays.LowerThirdPresets.TryGetValue(name,out var look)==true) SetDraft(look); };
        _font.LostFocus+=(_,_)=>CommitText();_nameColor.LostFocus+=(_,_)=>CommitText();_titleColor.LostFocus+=(_,_)=>CommitText();
        _backgroundColor.LostFocus+=(_,_)=>CommitText();_accentColor.LostFocus+=(_,_)=>CommitText();
        _logo.Checked+=(_,_)=> { if (!_loading) Edit(_draft with { ShowLogo=true }); };_logo.Unchecked+=(_,_)=> { if (!_loading) Edit(_draft with { ShowLogo=false }); };
        Loaded+=(_,_)=> { LoadDraft();if (_previewActive) StartPreview(); };
        Unloaded+=(_,_)=>DeactivatePreview();
    }
    private void Add(StackPanel root,string key,string label,double min,double max,double neutral,double step,string unit,Func<double,LowerThirdAppearance> update,double scale=1)
    {
        var c=new SliderValueControl { Header=label,Minimum=min,Maximum=max,DefaultValue=neutral,StepFrequency=step,Unit=unit,DisplayScale=scale };
        c.ValueChanged+=(_,v)=> { if (!_loading) Edit(update(v)); };_values[key]=c;root.Children.Add(c);
    }
    private void LoadDraft() { if (Studio is null) return;_saved.ItemsSource=Studio.Overlays.LowerThirdPresets.Keys.ToArray();SetDraft(Studio.Overlays.AppliedAppearance); }
    private void SetDraft(LowerThirdAppearance draft)
    {
        _loading=true;_draft=draft;
        try { foreach (var p in _values) p.Value.Value=(double)typeof(LowerThirdAppearance).GetProperty(p.Key)!.GetValue(draft)!;
            _preset.SelectedItem=draft.Preset;_anchor.SelectedItem=draft.Anchor;_font.Text=draft.FontFamily;_nameColor.Text=draft.NameColor;_titleColor.Text=draft.TitleColor;_logo.IsChecked=draft.ShowLogo;
            _namePicker.Color=HexColor.ParseOrDefault(draft.NameColor,Windows.UI.Color.FromArgb(255,244,247,250));_titlePicker.Color=HexColor.ParseOrDefault(draft.TitleColor,Windows.UI.Color.FromArgb(255,68,193,161));
            _backgroundColor.Text=draft.BackgroundColor;_accentColor.Text=draft.AccentColor;
            _backgroundPicker.Color=HexColor.ParseOrDefault(draft.BackgroundColor,Windows.UI.Color.FromArgb(255,12,17,24));_accentPicker.Color=HexColor.ParseOrDefault(draft.AccentColor,Windows.UI.Color.FromArgb(255,68,193,161)); }
        finally { _loading=false; }
        UpdatePreview();_status.Text="Draft — Apply look updates the live lower third.";
    }
    private void Edit(LowerThirdAppearance draft) { if (!draft.IsValid) return;_draft=draft;UpdatePreview();_status.Text="Draft — Apply look updates the live lower third."; }
    private bool CommitText()
    {
        var draft=_draft with { FontFamily=_font.Text.Trim(),NameColor=_nameColor.Text.Trim(),TitleColor=_titleColor.Text.Trim(),BackgroundColor=_backgroundColor.Text.Trim(),AccentColor=_accentColor.Text.Trim() };
        if (draft.IsValid) {
            _loading=true;
            try {
                _namePicker.Color=HexColor.ParseOrDefault(draft.NameColor,_namePicker.Color);
                _titlePicker.Color=HexColor.ParseOrDefault(draft.TitleColor,_titlePicker.Color);
                _backgroundPicker.Color=HexColor.ParseOrDefault(draft.BackgroundColor,_backgroundPicker.Color);
                _accentPicker.Color=HexColor.ParseOrDefault(draft.AccentColor,_accentPicker.Color);
            } finally { _loading=false; }
            Edit(draft);return true;
        }
        _status.Text="Enter a font family and six-digit hex colors. Previous valid draft retained.";return false;
    }
    private void StartPreview()
    {
        if (Studio is null || _coordinator is not null) return;
        _editor=new("preview:lower-third","Lower-third draft",new() { Lut="none" });_editor.LiveEditing=false;
        _editor.PropertyChanged+=(_,e)=> { if (_editor is not null && e.PropertyName==nameof(_editor.NativeSurface)) {_preview.SurfaceState=_editor.NativeSurface;_previewStatus.Text=_editor.NativeSurface.StatusLine;} };
        UpdatePreview();_coordinator=Studio.CreateAppearancePreview(_editor);
    }
    private void UpdatePreview()
    {
        if (_editor is null || Studio is null) return;
        var look=_compare.IsOn?Studio.BrandKit.LowerThirdAppearance:_draft;
        _editor.SetLowerThirdPreview(new(look,BrandColor:Studio.BrandKit.BrandColor,BackgroundColor:Studio.BrandKit.BackgroundColor,
            ImageUri:look?.ShowLogo==true?Studio.BrandKit.LogoAssetPath??"":"",Position:look?.Anchor??Studio.Overlays.LowerThirdPosition,FontFamily:Studio.BrandKit.FontFamily));
    }
}
