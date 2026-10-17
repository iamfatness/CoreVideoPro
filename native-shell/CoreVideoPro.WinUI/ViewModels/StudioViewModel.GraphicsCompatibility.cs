using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private bool _lastManagedGraphicsAuthority,_lastManagedGraphicsVisible;
    private void ObserveGraphicsState(NativeGraphicsState? state)
    {
        _graphicsWorkspace?.Observe(state);
        var authority=state?.FamilyAuthorities.GetValueOrDefault("lower-third")=="instances";
        var visible=authority && ManagedLowerThirdVisible;
        if(authority!=_lastManagedGraphicsAuthority) {_lastManagedGraphicsAuthority=authority;OnPropertyChanged(nameof(ManagedLowerThirdAuthority));}
        if(visible!=_lastManagedGraphicsVisible) {_lastManagedGraphicsVisible=visible;OnPropertyChanged(nameof(ManagedLowerThirdVisible));}
        if(authority) ProgramLowerThirdEnabled=visible;
    }
    public Task<bool> SaveGraphicsCueAsync(string name)=>GraphicsLibrary.SaveCueAsync(name,
        JsonSerializer.SerializeToElement(new {names=_sourceDisplayNames,secondaryLines=_sourceSecondaryLines}),JsonSerializer.SerializeToElement(BrandKit));
    public async Task<bool> CopyLegacyGraphicsToLibraryAsync()
    {
        var source=ResolveProgramLowerThirdSource(ProgramSceneRoutes);
        var appearance=Overlays.AppliedAppearance;
        var draft=new NativeGraphicsDraft(appearance.Preset,1,JsonSerializer.SerializeToElement(new {name=source?.SourceName ?? "Example guest",title=source is null?"":Services.SourceLowerThirdText.Preview(source.Title,source.Org)}),appearance,
            new() {In=new() {Type="slide",Direction="down",DurationMs=NormalizeLowerThirdTimingMs(LowerThirdBuildInMs),Easing="ease-out"},Out=new() {Type="slide",Direction="down",DurationMs=NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs),Easing="ease-in"}});
        var logo=appearance.ShowLogo?BrandKit.LogoAssetPath ?? "":"";
        if(logo.Length>0) {
            var reference=await GraphicsWorkspace.ImportImageAsync(logo);if(reference is null) {CommandStatus=GraphicsWorkspace.ImportStatus;return false;}
            draft=draft with {Image=reference};
        }
        return await GraphicsLibrary.ImportLegacyAsync(draft,source?.SourceId ?? "",JsonSerializer.SerializeToElement(new {names=_sourceDisplayNames,secondaryLines=_sourceSecondaryLines}),
            JsonSerializer.SerializeToElement(BrandKit),Overlays.LowerThirdPresets,appearance.ShowLogo?BrandKit.LogoAssetPath ?? "":"");
    }
    public bool ManagedLowerThirdAuthority => _bridge.LastSnapshot?.Graphics?.FamilyAuthorities.GetValueOrDefault("lower-third")=="instances";
    private IEnumerable<NativeGraphicsInstance> ManagedLowerThirds => (_bridge.LastSnapshot?.Graphics?.Instances ?? []).Where(i =>
        i.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" || _bridge.LastSnapshot?.Graphics?.Templates.Any(t=>t.TemplateId==i.TemplateId && t.TemplateVersion==i.TemplateVersion && t.Family=="lower-third")==true);
    public bool ManagedLowerThirdVisible => ManagedLowerThirds.Any(i=>i.ProgramRevision>0 && i.Visibility is not ("hidden" or "building-out"));
    public async Task<NativeGraphicsOperationResult> ApplyManagedLowerThirdIntentAsync(bool visible,bool rebuild=false)
    {
        var state=_bridge.LastSnapshot?.Graphics;
        if(!ManagedLowerThirdAuthority || state is null) return new() {Reason="Managed lower-third authority is not selected."};
        var targets=ManagedLowerThirds.ToArray();
        NativeGraphicsOperationResult result=new() {Accepted=!visible,Reason=visible?"Create or restore a lower-third cue first.":"Managed lower thirds are hidden."};
        async Task<NativeGraphicsOperationResult> Send(string action,Dictionary<string,object?> body) {
            body["schemaVersion"]=1;body["clientId"]="legacy-lower-third";body["requestId"]="legacy-"+Guid.NewGuid().ToString("N");body["coreEpoch"]=state.CoreEpoch;
            return await InvokeGraphicsActionAsync(action,JsonSerializer.Serialize(body,new JsonSerializerOptions(JsonSerializerDefaults.Web)));
        }
        if(!visible) {
            foreach(var item in targets.Where(i=>i.ProgramRevision>0 || i.ShowPending)) {
                result=await Send("graphics.instance.trigger",new() {["instanceId"]=item.InstanceId,["expectedRevision"]=item.Revision,["action"]="hide"});
                if(!result.Accepted) break;
            }
        } else if(!rebuild && ManagedLowerThirdVisible) result=new() {Accepted=true,Reason="Managed lower third is already on air."};
        else {
            var item=targets.OrderByDescending(i=>i.ProgramRevision>0).ThenByDescending(i=>i.CueRevision).FirstOrDefault();
            if(item is not null && rebuild) {
                var source=ResolveProgramLowerThirdSource(ProgramSceneRoutes);
                var binding=state.Sources.FirstOrDefault(s=>s.VideoOn && s.Identity?.SourceId==source?.SourceId)?.Identity;
                if(source is null || binding is null) result=new() {Reason="The rendered subject is not qualified for a managed lower third. Choose a current supported source."};
                else {
                    var fields=JsonSerializer.SerializeToElement(item.Fields,new JsonSerializerOptions(JsonSerializerDefaults.Web)).EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
                    fields["name"]=JsonSerializer.SerializeToElement(source.SourceName);
                    fields["title"]=JsonSerializer.SerializeToElement(Services.SourceLowerThirdText.Preview(source.Title,source.Org));
                    result=await Send("graphics.instance.update",new() {["instanceId"]=item.InstanceId,["expectedRevision"]=item.Revision,["target"]=item.ProgramRevision>0?"program":"cue",["fields"]=fields,["binding"]=binding});
                    if(result.Accepted && item.ProgramRevision==0) result=await Send("graphics.instance.trigger",new() {["instanceId"]=item.InstanceId,["expectedRevision"]=result.CurrentRevision,["action"]="show"});
                }
            } else if(item is {CueRevision:>0}) result=await Send("graphics.instance.trigger",new() {["instanceId"]=item.InstanceId,["expectedRevision"]=item.CueRevision,["target"]="cue",["action"]="show"});
            else result=new() {Reason="Prepare a lower-third Cue before using the existing Show control."};
        }
        CommandStatus=result.Reason;
        if(result.Accepted) {ProgramLowerThirdEnabled=visible;_programLowerThirdAutomationSuppressed=!visible;}
        return result;
    }
}
