using CoreVideoPro.MediaCore.Models;
using System.Text.Json;
namespace CoreVideoPro.MediaCore.Services;
public static class GraphicsCompatibilityPolicy
{
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
