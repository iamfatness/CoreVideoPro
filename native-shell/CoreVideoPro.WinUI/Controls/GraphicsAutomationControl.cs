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
    private static readonly string[] Modes = ["manual", "suggest", "rehearsal", "automatic"];
    private readonly ComboBox _mode=new() {Header="Mode",ItemsSource=new[] {"Manual", "Suggest", "Rehearsal", "Automatic"},SelectedIndex=0,HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ComboBox _target=new() {Header="Lower-third target",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly ComboBox _source=new() {Header="Subject",HorizontalAlignment=HorizontalAlignment.Stretch};
    private readonly CheckBox _enabled=new() {Content="Rule enabled",IsChecked=true},_share=new() {Content="Suppress during screen share",IsChecked=true};
    private readonly SliderValueControl _dwell=Seconds("Subject dwell",60,1.5),_duration=Seconds("Auto-hide after first draw",3600,8),_cooldown=Seconds("Minimum repeat interval",3600,0);
    private readonly TextBlock _state=Copy(""),_trace=Copy(""),_status=Copy(""),_scope=Copy("");
    private readonly List<Button> _buttons=[];
    private readonly List<Button> _cueButtons=[];
    private readonly List<string> _targets=[],_sources=[];
    private readonly GraphicsAutomationEditGuard _ruleEdits = new(), _modeEdits = new();
    private readonly TextBlock _modeHelp=Copy(""), _editNotice=Copy("");
    private readonly Button _applyMode, _applyRule, _arm, _cueSuggestion, _removeRule, _restore, _saveRules;
    private string _catalog="",_loadedTarget="";
    private bool _loading,_working;
    public GraphicsAutomationControl(StudioViewModel studio)
    {
        _studio=studio;_session=studio.GraphicsWorkspace;_library=studio.GraphicsLibrary;
        var panel=new StackPanel {Spacing=8};
        panel.Children.Add(Copy("Introduce the guest currently visible on Program. Choose a mode, configure a guest-introduction cue, then Arm when ready. Video Takes remain separate."));
        panel.Children.Add(_state);panel.Children.Add(new TextBlock {Text="1. Choose behavior",FontSize=16});panel.Children.Add(_mode);
        panel.Children.Add(_modeHelp);
        _applyMode=Button("Apply mode",async()=> {
            if(_modeEdits.Conflict)return false;
            var accepted=await _session.AutomationControlAsync("set-mode",SelectedMode);
            if(accepted)_modeEdits.Reload();return accepted;
        });panel.Children.Add(_applyMode);
        panel.Children.Add(Copy("Changing mode disarms it. Reapplying the same mode keeps its armed state."));
        var arm=new StackPanel {Orientation=Orientation.Horizontal,Spacing=6};
        _arm=Button("Arm",()=>_session.AutomationControlAsync("arm"));arm.Children.Add(_arm);arm.Children.Add(Button("Disarm",()=>_session.AutomationControlAsync("disarm")));
        _cueSuggestion=Button("Cue selected suggestion to Preview",()=>_session.CueSuggestionAsync(Target));
        panel.Children.Add(_editNotice);
        panel.Children.Add(Button("Discard edits and reload current settings",()=> { _ruleEdits.Reload();_modeEdits.Reload();Refresh();return Task.FromResult(true); },"Current settings reloaded. No native request was sent."));
        panel.Children.Add(new TextBlock {Text="2. Configure an introduction",FontSize=16});
        panel.Children.Add(_target);panel.Children.Add(_scope);panel.Children.Add(_source);panel.Children.Add(_enabled);
        panel.Children.Add(Copy("Only uniquely drawn guest follows the single eligible guest visible on Program. For a panel with several visible guests, choose a specific subject."));
        var timing=new StackPanel {Spacing=8};timing.Children.Add(_share);timing.Children.Add(_dwell);timing.Children.Add(_duration);timing.Children.Add(_cooldown);
        timing.Children.Add(Copy("Auto-hide 0 holds until departure, suppression or Hide. Each guest incarnation reveals once; returning to the same guest does not replay the entrance."));
        panel.Children.Add(new Expander {Header="Timing and screen-share behavior",Content=timing,HorizontalAlignment=HorizontalAlignment.Stretch});
        _applyRule=Button("Apply rule — disarmed",ApplyRule);panel.Children.Add(_applyRule);
        panel.Children.Add(new TextBlock {Text="3. Activate when ready",FontSize=16});panel.Children.Add(arm);panel.Children.Add(_cueSuggestion);
        panel.Children.Add(Copy("Disarm stops pending actions and leaves visible graphics on air. Hide and disarm removes automatic graphics."));
        panel.Children.Add(Button("Hide automatic graphics and disarm",()=>_session.AutomationControlAsync("hide-automatic")));
        var overrides=new StackPanel {Spacing=8};
        overrides.Children.Add(Copy("Manual Cue, Show, Hide or Update holds that rule for this session. Clear inhibits automation until Arm. Rejoining disarms it."));
        overrides.Children.Add(CueButton("Hold automation for this cue",()=>_session.AutomationControlAsync("hold",instanceId:Target)));
        overrides.Children.Add(CueButton("Resume automation for this cue",()=>_session.AutomationControlAsync("resume",instanceId:Target)));
        overrides.Children.Add(Button("Hold all automation",()=>_session.AutomationControlAsync("hold")));
        overrides.Children.Add(Button("Resume all automation",()=>_session.AutomationControlAsync("resume")));
        _removeRule=Button("Remove selected rule",RemoveRule);overrides.Children.Add(_removeRule);
        panel.Children.Add(new Expander {Header="Manual overrides and rule removal",Content=overrides,HorizontalAlignment=HorizontalAlignment.Stretch});
        var persistence=new StackPanel {Spacing=8};
        _saveRules=Button("Save rule configuration",()=>_library.SaveAutomationRulesAsync(),"Applied rule configuration saved. Activation is unchanged.");persistence.Children.Add(_saveRules);
        _restore=Button("Restore saved rules — unarmed",()=>_library.RestoreAutomationRulesAsync(_studio.GraphicsAutomationConfiguration));persistence.Children.Add(_restore);
        panel.Children.Add(new Expander {Header="Save and restore applied rules",Content=persistence,HorizontalAlignment=HorizontalAlignment.Stretch});
        panel.Children.Add(_status);panel.Children.Add(new Expander {Header="Native decision trace",Content=_trace,HorizontalAlignment=HorizontalAlignment.Stretch});
        Content=panel;
        _target.SelectionChanged+=(_,_)=>{
            if(_loading)return;
            if(_ruleEdits.Dirty) {
                _loading=true;_target.SelectedIndex=_targets.IndexOf(_loadedTarget);_loading=false;
                _status.Text="Apply or discard your rule edits before choosing another cue.";
            } else Refresh();
        };
        _mode.SelectionChanged+=(_,_)=>{if(!_loading){_modeEdits.MarkEdited();_modeHelp.Text=GraphicsAutomationEditGuard.ModeHelp(SelectedMode);RefreshEnabled();}};
        _source.SelectionChanged+=(_,_)=>MarkRuleEdited();
        _enabled.Checked+=(_,_)=>MarkRuleEdited();_enabled.Unchecked+=(_,_)=>MarkRuleEdited();
        _share.Checked+=(_,_)=>MarkRuleEdited();_share.Unchecked+=(_,_)=>MarkRuleEdited();
        _dwell.ValueChanged+=(_,_)=>MarkRuleEdited();_duration.ValueChanged+=(_,_)=>MarkRuleEdited();_cooldown.ValueChanged+=(_,_)=>MarkRuleEdited();
        Refresh();
    }
    private string SelectedMode=>_mode.SelectedIndex>=0 && _mode.SelectedIndex<Modes.Length?Modes[_mode.SelectedIndex]:"manual";
    private void MarkRuleEdited() {if(!_loading){_ruleEdits.MarkEdited();RefreshEnabled();}}
    private string Target=>_target.SelectedIndex>=0 && _target.SelectedIndex<_targets.Count?_targets[_target.SelectedIndex]:"";
    private static TextBlock Copy(string text)=>new() {Text=text,TextWrapping=TextWrapping.Wrap,FontSize=12};
    private static SliderValueControl Seconds(string header,double max,double value)=>new() {Header=header,Unit="s",Minimum=0,Maximum=max,StepFrequency=.1,DefaultValue=value,Value=value,Compact=true,Help="Exact values are available beside the slider."};
    private Button Button(string label,Func<Task<bool>> action,string success="Native request accepted. Review the current state and trace below.")
    {
        var button=new Button {Content=label,HorizontalAlignment=HorizontalAlignment.Stretch};_buttons.Add(button);
        button.Click+=async(_,_)=>{
            if(_working || _session.Busy) return;
            _working=true;RefreshEnabled();
            try {var accepted=await action();_status.Text=accepted?success:_session.Status+" · "+_library.Status;}
            catch(Exception ex) when(ex is not OutOfMemoryException) {_status.Text=ex.Message;}
            finally {_working=false;Refresh();}
        };return button;
    }
    private Button CueButton(string label,Func<Task<bool>> action)
    {
        var button=Button(label,()=>Target.Length>0?action():Task.FromResult(false));_cueButtons.Add(button);return button;
    }
    private async Task<bool> ApplyRule()
    {
        if(_ruleEdits.Conflict || _session.State is not { } state || Target.Length==0) return false;
        var target=state.Instances.First(i=>i.InstanceId==Target);
        var rules=(state.Automation?.Configuration.Rules ?? []).Where(r=>r.InstanceId!=Target).ToList();
        if(rules.Count>=8) {_status.Text="Eight rules are already configured.";return false;}
        var old=state.Automation?.Configuration.Rules.FirstOrDefault(r=>r.InstanceId==Target);
        rules.Add(new() {Id=old?.Id ?? "rule-"+Guid.NewGuid().ToString("N"),InstanceId=Target,SceneId=target.Scope=="scene"?target.SceneId:"",SourceId=_source.SelectedIndex>=0 && _source.SelectedIndex<_sources.Count?_sources[_source.SelectedIndex]:"",Enabled=_enabled.IsChecked==true,SuppressScreenShare=_share.IsChecked==true,DwellMs=_dwell.Value*1000,DurationMs=_duration.Value*1000,CooldownMs=_cooldown.Value*1000});
        var accepted=await _session.ConfigureAutomationAsync(_studio.GraphicsAutomationConfiguration(rules));
        if(accepted)_ruleEdits.Reload();return accepted;
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
        SelectSource(rule?.SourceId ?? "");
        _loadedTarget=Target;
    }
    private void SelectSource(string id)
    {
        if(!_sources.Contains(id)) {_sources.Add(id);_source.Items.Add("Unavailable guest · "+id);}
        _source.SelectedIndex=_sources.IndexOf(id);
    }
    private void RefreshEnabled()
    {
        var available=!_working&&!_session.Busy&&_session.State?.Automation is not null;
        foreach(var button in _buttons)button.IsEnabled=available;
        var automation=_session.State?.Automation;
        var hasTarget=_session.State?.Instances.Any(i=>i.InstanceId==Target)==true;
        foreach(var button in _cueButtons)button.IsEnabled=available&&hasTarget;
        _applyMode.IsEnabled=available&&!_modeEdits.Conflict&&_modeEdits.Dirty;
        _applyRule.IsEnabled=available&&hasTarget&&!_ruleEdits.Conflict;
        _removeRule.IsEnabled=available&&!_ruleEdits.Dirty&&automation?.Configuration.Rules.Any(r=>r.InstanceId==Target)==true;
        _restore.IsEnabled=available&&!_ruleEdits.Dirty&&!_modeEdits.Dirty;
        _saveRules.IsEnabled=available&&!_ruleEdits.Dirty&&!_modeEdits.Dirty;
        _arm.IsEnabled=available&&!_ruleEdits.Dirty&&!_modeEdits.Dirty&&(automation?.Mode is "automatic" or "rehearsal")&&automation.Configuration.Rules.Any(r=>r.Enabled);
        _cueSuggestion.IsEnabled=available&&automation?.Suggestions.Any(s=>s.InstanceId==Target)==true;
        foreach(var input in new Microsoft.UI.Xaml.Controls.Control[] {_mode,_target,_source,_enabled,_share,_dwell,_duration,_cooldown})input.IsEnabled=available;
        _editNotice.Text=_ruleEdits.Conflict||_modeEdits.Conflict?"Settings changed elsewhere or the core reconnected. Your edits are kept; discard and reload before applying.":_ruleEdits.Dirty||_modeEdits.Dirty?"Unsaved edits here have no live effect. Apply sends them to the core; Save rule configuration keeps applied rules for later.":"Showing the core's current settings. Restored rules stay unarmed.";
    }
    public void Refresh()
    {
        var state=_session.State;var automation=state?.Automation;_loading=true;
        try {
            var targets=(state?.Instances ?? []).Where(i=>i.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" || state!.Templates.Any(t=>t.TemplateId==i.TemplateId && t.TemplateVersion==i.TemplateVersion && t.Family=="lower-third")).ToArray();
            var sources=(state?.Sources ?? []).Where(s=>s.Identity is not null && s.VideoOn).ToArray();
            var catalog=string.Join('|',targets.Select(i=>i.InstanceId+":"+i.TemplateId+":"+i.Label+":"+i.Scope+":"+i.SceneId))+"/"+string.Join('|',sources.Select(s=>s.Identity+":"+s.DisplayName));
            if(catalog!=_catalog) {
                _catalog=catalog;var selected=Target;_targets.Clear();_target.Items.Clear();
                foreach(var target in targets) {_targets.Add(target.InstanceId);_target.Items.Add((target.Label.Length>0?target.Label:target.TemplateId)+" · "+(target.Scope=="global"?"Global":target.SceneId));}
                if(_ruleEdits.Dirty && selected.Length>0 && !_targets.Contains(selected)) {_targets.Add(selected);_target.Items.Add("Unavailable cue · "+selected);}
                _target.SelectedIndex=Math.Max(0,_targets.IndexOf(selected));
                var priorSource=_source.SelectedIndex>=0 && _source.SelectedIndex<_sources.Count?_sources[_source.SelectedIndex]:"";
                _sources.Clear();_sources.Add("");_source.Items.Clear();_source.Items.Add("Only uniquely drawn guest");
                foreach(var source in sources) {_sources.Add(source.Identity!.SourceId);_source.Items.Add(source.DisplayName+" · "+source.Identity.SourceId);}
                SelectSource(priorSource);
            }
            if(_ruleEdits.Observe(state?.CoreEpoch ?? "",Target,automation is null?null:automation with {Mode="manual"})) LoadRule();
            if(_modeEdits.Observe(state?.CoreEpoch ?? "","",automation is null?null:new() {Mode=automation.Mode}))
                _mode.SelectedIndex=Array.IndexOf(Modes,automation?.Mode ?? "manual");
            _modeHelp.Text=GraphicsAutomationEditGuard.ModeHelp(SelectedMode);
            _state.Text=automation is null?"Native automation is unavailable in this core.":$"Native: {automation.Mode} · {(automation.Armed?"armed":"disarmed")} · {(automation.Inhibited?"Inhibited — explicit Arm required":automation.ManualHold?"all held":automation.HeldInstances.Count+" cue holds")}";
            _trace.Text=automation?.Trace.Count>0?string.Join("\n",automation.Trace.TakeLast(12).Reverse().Select(t=>$"{t.Sequence} · {t.RuleId} · {t.State} · {t.Reason} · r{t.Revision}")):"No native decisions observed. Accepted requests and native draw submission are separate facts; neither proves receiver delivery.";
            RefreshEnabled();
        } finally {_loading=false;}
    }
}
