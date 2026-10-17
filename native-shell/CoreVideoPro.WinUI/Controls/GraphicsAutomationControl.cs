using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Explicit native rule configuration. Snapshot facts alone establish
/// arming, holds and rendering; editing these controls never activates a rule.</summary>
public sealed class GraphicsAutomationControl : UserControl
{
    private readonly StudioViewModel _studio;
    private readonly GraphicsWorkspaceSession _session;
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly ComboBox _mode=new() {Header="Mode",ItemsSource=new[] {"manual","suggest","rehearsal","automatic"},SelectedIndex=0,HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ComboBox _target=new() {Header="Lower-third target",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ComboBox _source=new() {Header="Subject",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly CheckBox _enabled=new() {Content="Rule enabled",IsChecked=true},_share=new() {Content="Suppress during screen share",IsChecked=true};
    private readonly SliderValueControl _dwell=Seconds("Subject dwell",60,1.5),_duration=Seconds("Auto-hide after first draw",3600,8),_cooldown=Seconds("Minimum repeat interval",3600,0);
    private readonly TextBlock _state=Copy(""),_trace=Copy(""),_status=Copy(""),_scope=Copy("");
    private readonly List<Button> _buttons=[];
    private readonly List<string> _targets=[],_sources=[];
    private string _catalog="",_loadedTarget="";
    private long _loadedRevision=-1;
    private bool _loading,_working;
    public GraphicsAutomationControl(StudioViewModel studio)
    {
        _studio=studio;_session=studio.GraphicsWorkspace;_library=studio.GraphicsLibrary;
        var panel=new StackPanel {Spacing=8};
        panel.Children.Add(Copy("Automatic introductions use the exact guest drawn by the native compositor. Multiple visible guests need an explicit subject. Saving is separate from activation; video Takes remain independent."));
        panel.Children.Add(_state);panel.Children.Add(_mode);
        panel.Children.Add(Button("Apply mode — disarmed",()=>_session.AutomationControlAsync("set-mode",_mode.SelectedItem as string ?? "manual")));
        var arm=new StackPanel {Orientation=Orientation.Horizontal,Spacing=6};
        arm.Children.Add(Button("Arm",()=>_session.AutomationControlAsync("arm")));arm.Children.Add(Button("Disarm",()=>_session.AutomationControlAsync("disarm")));panel.Children.Add(arm);
        panel.Children.Add(Button("Cue selected suggestion to Preview",()=>_session.CueSuggestionAsync(Target)));
        panel.Children.Add(_target);panel.Children.Add(_scope);panel.Children.Add(_source);panel.Children.Add(_enabled);panel.Children.Add(_share);
        panel.Children.Add(_dwell);panel.Children.Add(_duration);panel.Children.Add(_cooldown);
        panel.Children.Add(Copy("Duration 0 holds until departure, suppression or an explicit Hide. One reveal per qualified guest incarnation; returning to the same guest does not replay the entrance."));
        panel.Children.Add(Button("Apply rule — disarmed",ApplyRule));
        panel.Children.Add(Button("Remove selected rule",RemoveRule));
        panel.Children.Add(Button("Hold automation for this cue",()=>_session.AutomationControlAsync("hold",instanceId:Target)));
        panel.Children.Add(Button("Resume automation for this cue",()=>_session.AutomationControlAsync("resume",instanceId:Target)));
        panel.Children.Add(Button("Hold all automation",()=>_session.AutomationControlAsync("hold")));
        panel.Children.Add(Button("Resume all automation",()=>_session.AutomationControlAsync("resume")));
        panel.Children.Add(Button("Hide automatic graphics and disarm",()=>_session.AutomationControlAsync("hide-automatic")));
        panel.Children.Add(Button("Save rule configuration",()=>_library.SaveAutomationRulesAsync()));
        panel.Children.Add(Button("Restore saved rules — unarmed",()=>_library.RestoreAutomationRulesAsync(_studio.GraphicsAutomationConfiguration)));
        panel.Children.Add(Copy("Manual Cue, Show, Hide or Update holds that rule for this session. Clear inhibits automation until an explicit Arm. Rejoining disarms it. Rehearsal prepares Preview only."));
        panel.Children.Add(_status);panel.Children.Add(new Expander {Header="Native decision trace",Content=_trace,HorizontalAlignment=HorizontalAlignment.Stretch});
        Content=panel;_target.SelectionChanged+=(_,_)=>{if(!_loading) LoadRule();};Refresh();
    }
    private string Target=>_target.SelectedIndex>=0 && _target.SelectedIndex<_targets.Count?_targets[_target.SelectedIndex]:"";
    private static TextBlock Copy(string text)=>new() {Text=text,TextWrapping=TextWrapping.Wrap,FontSize=12};
    private static SliderValueControl Seconds(string header,double max,double value)=>new() {Header=header,Unit="s",Minimum=0,Maximum=max,StepFrequency=.1,DefaultValue=value,Value=value,Compact=true,Help="Exact values are available beside the slider."};
    private Button Button(string label,Func<Task<bool>> action)
    {
        var button=new Button {Content=label,HorizontalAlignment=HorizontalAlignment.Stretch};_buttons.Add(button);
        button.Click+=async(_,_)=>{
            if(_working || _session.Busy) return;
            _working=true;RefreshEnabled();
            try {var accepted=await action();_status.Text=accepted?"Native request accepted. Review the current state and trace below.":_session.Status+" · "+_library.Status;}
            catch(Exception ex) when(ex is not OutOfMemoryException) {_status.Text=ex.Message;}
            finally {_working=false;Refresh();}
        };return button;
    }
    private Task<bool> ApplyRule()
    {
        if(_session.State is not { } state || Target.Length==0) return Task.FromResult(false);
        var target=state.Instances.First(i=>i.InstanceId==Target);
        var rules=(state.Automation?.Configuration.Rules ?? []).Where(r=>r.InstanceId!=Target).ToList();
        if(rules.Count>=8) {_status.Text="Eight rules are already configured.";return Task.FromResult(false);}
        var old=state.Automation?.Configuration.Rules.FirstOrDefault(r=>r.InstanceId==Target);
        rules.Add(new() {Id=old?.Id ?? "rule-"+Guid.NewGuid().ToString("N"),InstanceId=Target,SceneId=target.Scope=="scene"?target.SceneId:"",SourceId=_source.SelectedIndex>=0 && _source.SelectedIndex<_sources.Count?_sources[_source.SelectedIndex]:"",Enabled=_enabled.IsChecked==true,SuppressScreenShare=_share.IsChecked==true,DwellMs=_dwell.Value*1000,DurationMs=_duration.Value*1000,CooldownMs=_cooldown.Value*1000});
        return _session.ConfigureAutomationAsync(_studio.GraphicsAutomationConfiguration(rules));
    }
    private Task<bool> RemoveRule()=>_session.ConfigureAutomationAsync(_studio.GraphicsAutomationConfiguration((_session.State?.Automation?.Configuration.Rules ?? []).Where(r=>r.InstanceId!=Target).ToArray()));
    private void LoadRule()
    {
        if(_session.State is not { } state) return;
        var rule=state.Automation?.Configuration.Rules.FirstOrDefault(r=>r.InstanceId==Target);
        var target=state.Instances.FirstOrDefault(i=>i.InstanceId==Target);
        _scope.Text=target?.Scope=="scene"?"Scene scope: "+target.SceneId:"Global scope";
        _enabled.IsChecked=rule?.Enabled ?? true;_share.IsChecked=rule?.SuppressScreenShare ?? true;
        _dwell.Value=(rule?.DwellMs ?? 1500)/1000;_duration.Value=(rule?.DurationMs ?? 8000)/1000;_cooldown.Value=(rule?.CooldownMs ?? 0)/1000;
        _source.SelectedIndex=Math.Max(0,_sources.IndexOf(rule?.SourceId ?? ""));
        if(rule is {SourceId.Length:>0} && !_sources.Contains(rule.SourceId)) {_sources.Add(rule.SourceId);_source.Items.Add("Unavailable · "+rule.SourceId);_source.SelectedIndex=_sources.Count-1;}
        _loadedTarget=Target;_loadedRevision=state.Automation?.Revision ?? 0;
    }
    private void RefreshEnabled() {foreach(var button in _buttons) button.IsEnabled=!_working&&!_session.Busy&&_session.State?.Automation is not null;}
    public void Refresh()
    {
        var state=_session.State;var automation=state?.Automation;_loading=true;
        try {
            var targets=(state?.Instances ?? []).Where(i=>i.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" || state!.Templates.Any(t=>t.TemplateId==i.TemplateId && t.TemplateVersion==i.TemplateVersion && t.Family=="lower-third")).ToArray();
            var sources=(state?.Sources ?? []).Where(s=>s.Identity is not null && s.VideoOn).ToArray();
            var catalog=string.Join('|',targets.Select(i=>i.InstanceId+":"+i.TemplateId))+"/"+string.Join('|',sources.Select(s=>s.Identity+":"+s.DisplayName));
            if(catalog!=_catalog) {
                _catalog=catalog;var selected=Target;_targets.Clear();_target.Items.Clear();
                foreach(var target in targets) {_targets.Add(target.InstanceId);_target.Items.Add((target.Label.Length>0?target.Label:target.TemplateId)+" · "+(target.Scope=="global"?"Global":target.SceneId));}
                _target.SelectedIndex=Math.Max(0,_targets.IndexOf(selected));
                var priorSource=_source.SelectedIndex>=0 && _source.SelectedIndex<_sources.Count?_sources[_source.SelectedIndex]:"";
                _sources.Clear();_sources.Add("");_source.Items.Clear();_source.Items.Add("Only uniquely drawn guest");
                foreach(var source in sources) {_sources.Add(source.Identity!.SourceId);_source.Items.Add(source.DisplayName+" · "+source.Identity.SourceId);}
                _source.SelectedIndex=Math.Max(0,_sources.IndexOf(priorSource));
            }
            if(_loadedTarget!=Target || _loadedRevision!=(automation?.Revision ?? 0)) LoadRule();
            _state.Text=automation is null?"Native automation is unavailable in this core.":$"Native: {automation.Mode} · {(automation.Armed?"armed":"disarmed")} · {(automation.Inhibited?"Clear inhibited":automation.ManualHold?"all held":automation.HeldInstances.Count+" cue holds")}";
            _trace.Text=automation?.Trace.Count>0?string.Join("\n",automation.Trace.TakeLast(12).Reverse().Select(t=>$"{t.Sequence} · {t.RuleId} · {t.State} · {t.Reason} · r{t.Revision}")):"No native decisions observed. Accepted requests and native draw submission are separate facts; neither proves receiver delivery.";
            RefreshEnabled();
        } finally {_loading=false;}
    }
}
