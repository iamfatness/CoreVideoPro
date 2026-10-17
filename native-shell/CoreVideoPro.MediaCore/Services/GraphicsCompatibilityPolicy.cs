using CoreVideoPro.MediaCore.Models;
using System.Text.Json;
namespace CoreVideoPro.MediaCore.Services;
public static class GraphicsCompatibilityPolicy
{
    public static bool AutomationEnabled(NativeGraphicsState? state)=>state?.Automation is {Mode:"automatic",Armed:true,Inhibited:false,ManualHold:false} automation &&
        automation.Configuration.Rules.Any(r=>r.Enabled && !automation.HeldInstances.Contains(r.InstanceId));
    public static async Task<NativeGraphicsOperationResult> SetAutomationEnabledAsync(GraphicsWorkspaceSession session,bool enabled)
    {
        if(session.State?.Automation is not { } automation) return new() {Reason="Native graphics automation is unavailable."};
        if(enabled && !automation.Configuration.Rules.Any(r=>r.Enabled)) return new() {Reason="Configure an enabled lower-third rule in Graphics before arming automatic introductions."};
        if(enabled && (automation.ManualHold || automation.Configuration.Rules.Where(r=>r.Enabled).All(r=>automation.HeldInstances.Contains(r.InstanceId))))
            return new() {Reason="A manual hold is active. Use Resume automation in Graphics before arming introductions."};
        var accepted=await session.AutomationControlAsync(enabled?"enable-automatic":"hide-automatic",enabled?"automatic":"");
        return new() {Accepted=accepted,Reason=session.Status};
    }
    public static IEnumerable<NativeGraphicsInstance> LowerThirdsForScene(NativeGraphicsState? state,string sceneId)=>
        (state?.Instances ?? []).Where(i=>(i.Scope=="global" || i.SceneId==sceneId) &&
            (i.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" || state!.Templates.Any(t=>t.TemplateId==i.TemplateId && t.TemplateVersion==i.TemplateVersion && t.Family=="lower-third")));
    public static IReadOnlyDictionary<string,JsonElement>? SubjectFields(NativeGraphicsInstance instance,NativeGraphicsTemplate? template,string name,string secondary)
    {
        var fields=JsonSerializer.SerializeToElement(instance.Fields,new JsonSerializerOptions(JsonSerializerDefaults.Web)).EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone(),StringComparer.Ordinal);
        if(template?.Runtime=="declarative") {
            foreach(var key in fields.Keys.ToArray()) if(!template.Fields.Any(f=>f.Id==key)) fields.Remove(key);
            if(!template.Fields.Any(f=>f.Id=="name"&&f.Type=="string")) return null;
        }
        fields["name"]=JsonSerializer.SerializeToElement(name);
        if(template?.Runtime!="declarative" || template.Fields.Any(f=>f.Id=="title"&&f.Type=="string")) fields["title"]=JsonSerializer.SerializeToElement(secondary);
        return fields;
    }
}
