using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

// Isolated stdin/stdout core only. Never attaches to the installed app, meeting,
// camera, API port or production preferences. Receiver/display proof is separate.
if (args.Length is <2 or >3) throw new ArgumentException("Usage: GraphicsStarterProbe <native-exe> <report.json> [test-logo.png]");
var reportPath=Path.GetFullPath(args[1]);Directory.CreateDirectory(Path.GetDirectoryName(reportPath)!);
var json=new JsonSerializerOptions(JsonSerializerDefaults.Web) {DefaultIgnoreCondition=JsonIgnoreCondition.WhenWritingNull};
var start=new ProcessStartInfo(Path.GetFullPath(args[0])) {UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true,RedirectStandardOutput=true,RedirectStandardError=true};
foreach(var key in start.Environment.Keys.Where(k=>k.StartsWith("COREVIDEO_",StringComparison.OrdinalIgnoreCase)).ToArray())start.Environment.Remove(key);
using var core=Process.Start(start) ?? throw new InvalidOperationException("Isolated core did not start.");
var stderr=File.WriteAllTextAsync(reportPath+".native.log","");await stderr;
using var log=new StreamWriter(reportPath+".native.log");
var errors=Task.Run(async()=>{while(await core.StandardError.ReadLineAsync() is { } line)await log.WriteLineAsync(line);});
var replies=new ConcurrentDictionary<string,TaskCompletionSource<JsonElement>>();
var previews=new ConcurrentDictionary<string,JsonElement>();
var reader=Task.Run(async()=>{while(await core.StandardOutput.ReadLineAsync() is { } line) {
    using var document=JsonDocument.Parse(line);var root=document.RootElement;
    if(root.TryGetProperty("id",out var id) && id.ValueKind==JsonValueKind.String && replies.TryRemove(id.GetString()!,out var pending))pending.TrySetResult(root.Clone());
    if(root.TryGetProperty("type",out var type) && type.GetString()=="grade-preview")previews[root.GetProperty("instanceId").GetString()!]=root.Clone();
}});
var sequence=0;NativeGraphicsState? state=null;var checks=new List<string>();
var previousAssetRoot=Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT");
Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT",Path.Combine(Path.GetDirectoryName(reportPath)!,"asset-cache"));
async Task<JsonElement> Request(Dictionary<string,object?> body) {
    var id="starter-"+Interlocked.Increment(ref sequence);body["id"]=id;var pending=new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);replies[id]=pending;
    await core.StandardInput.WriteLineAsync(JsonSerializer.Serialize(body,json));await core.StandardInput.FlushAsync();
    var reply=await pending.Task.WaitAsync(TimeSpan.FromSeconds(15));
    if(!reply.GetProperty("ok").GetBoolean())throw new InvalidOperationException(reply.GetRawText());
    if(reply.TryGetProperty("snapshot",out var snapshot))state=JsonSerializer.Deserialize<NativeGraphicsState>(snapshot.GetProperty("graphics"),json);
    return reply;
}
async Task Sync()=>await Request(new() { ["type"]="media-core-sync",["commands"]=Array.Empty<object>() });
try {
    await Sync();
    await Request(new() { ["type"]="graphics-template-list",["graphics"]=new {schemaVersion=1,coreEpoch=state!.CoreEpoch,clientId="starter-qa",requestId="starter-catalog",includeThumbnails=true} });
    var thumbnails=Stopwatch.StartNew();
    while(thumbnails.Elapsed<TimeSpan.FromSeconds(5) && state!.Templates.Any(t=>t.Thumbnail?.State!="ready")) {await Task.Delay(100);await Sync();}
    await File.WriteAllTextAsync(Path.Combine(Path.GetDirectoryName(reportPath)!,"catalog.json"),JsonSerializer.Serialize(state,json));
    await File.WriteAllTextAsync(Path.Combine(Path.GetDirectoryName(reportPath)!,"starter-thumbnails.json"),JsonSerializer.Serialize(
        GraphicsShowStarters.All.Select(s=>new {id=s.Id,graphics=GraphicsShowStarters.Prepare(s,state!)[0].Draft.ForPreview()}),json));
    var session=new GraphicsWorkspaceSession(async(action,body,_)=>{
        await Request(new() {["type"]=action.Replace('.','-'),["graphics"]=JsonDocument.Parse(body).RootElement.Clone()});
        return state?.LastResult ?? throw new InvalidOperationException("Native graphics result missing.");
    },()=>state);
    NativeGraphicsImageReference? logo=null;NativeGraphicsTemplate? logoTemplate=null;
    if(args.Length==3) {
        session.Observe(state);logo=await session.ImportImageAsync(Path.GetFullPath(args[2]));
        if(logo is null)throw new InvalidOperationException(session.ImportStatus);
        logoTemplate=state!.Templates.Single(t=>t.TemplateId==logo.TemplateId && t.TemplateVersion==logo.TemplateVersion);
    }
    foreach(var starter in GraphicsShowStarters.All) {
        session.Observe(state);var graphics=GraphicsShowStarters.Prepare(starter,state!);
        graphics=GraphicsShowStarters.WithBranding(graphics,"My weekly show","#2873AA");
        if(logo is not null)graphics=GraphicsShowStarters.WithLogo(graphics,logoTemplate!,logo);
        foreach(var graphic in graphics) {
            session.PreviewStarter(graphic);if(session.PreviewValidation.Length>0)throw new InvalidOperationException(session.PreviewValidation);
            var id="starter-preview";var revision=++sequence;
            var wire=new LowerThirdPreview(null,Graphics:graphic.Draft.ForPreview());
            var request=new Dictionary<string,object?> {["type"]="set-grade-preview",["instanceId"]=id,["sourceId"]="preview:lower-third",["revision"]=revision,["enabled"]=true,["lowerThirdPreview"]=wire};
            // Actual model with the transport's null-ignore policy must keep this key.
            if(JsonSerializer.SerializeToElement(wire,json).GetProperty("appearance").ValueKind!=JsonValueKind.Null)throw new InvalidOperationException("Missing appearance null.");
            await Request(request);var watch=Stopwatch.StartNew();var ready=false;
            while(watch.Elapsed<TimeSpan.FromSeconds(2.8)) {
                await Sync();
                if(previews.TryGetValue(id,out var observation) && observation.GetProperty("revision").GetInt64()==revision && observation.GetProperty("status").GetString()=="ready" &&
                    observation.TryGetProperty("texture",out var texture) && texture.GetProperty("width").GetInt32()>0 && texture.GetProperty("sharedHandleHex").GetString() is {Length:>2}) {ready=true;break;}
                await Task.Delay(35);
            }
            if(!ready)throw new InvalidOperationException($"Native preview did not become ready: {starter.Name}/{graphic.Name}");
            if(state!.Instances.Count!=0)throw new InvalidOperationException("Trying a starter created native instances.");
            request["enabled"]=false;await Request(request);
            checks.Add($"{starter.Name}/{graphic.Name}: managed wire accepted, native private texture ready, no instances");
        }
        var store=new GraphicsDocumentStore(Path.Combine(Path.GetDirectoryName(reportPath)!,starter.Id+".json"));
        var library=new GraphicsWorkspaceLibrary(store,session);
        session.Observe(state);var adoption=await GraphicsStarterWorkflow.AddAsync(session,library,graphics,true,true);
        if(!adoption.Added || !adoption.Saved || !adoption.StudioAssigned)throw new InvalidOperationException(adoption.Message);
        if(state!.Instances.Count!=4 || state.Instances.Any(i=>i.CueRevision!=0 || i.ProgramRevision!=0))throw new InvalidOperationException("Starter was not added hidden.");
        var loaded=await store.LoadAsync();if(loaded.Document.Cues.Count!=4 || loaded.Document.StudioLowerThird is null)throw new InvalidOperationException("Starter persistence or assignment failed.");
        checks.Add(starter.Name+": one explicit action saves four named hidden cues and assigns the lower-third look");
        if(library.Document.StudioLowerThird?.Cue.Name!=starter.Name+" · Guest introduction" || !library.Document.StudioLowerThird.FollowProgramSource || state.Instances.Any(i=>i.ProgramRevision>0))
            throw new InvalidOperationException("Explicit Studio assignment lost the show-look name or changed Program.");
        checks.Add(starter.Name+": assigned show name and Program guest-text policy retained without showing anything");
        if(!await library.ClearStudioLowerThirdAsync())throw new InvalidOperationException(library.Status);
        foreach(var item in state.Instances.ToArray()) {session.Select(item.InstanceId);if(!await session.RemoveAsync())throw new InvalidOperationException(session.Status);}
        foreach(var cue in loaded.Document.Cues)if(!await library.RestoreAsync(cue))throw new InvalidOperationException(library.Status);
        if(state!.Instances.Count!=4 || state.Instances.Any(i=>i.CueRevision!=0 || i.ProgramRevision!=0))throw new InvalidOperationException("Saved show did not restore hidden.");
        checks.Add(starter.Name+": saved cues restored hidden with no live assignment or automation activation");
        foreach(var item in state.Instances.ToArray()) {session.Select(item.InstanceId);if(!await session.RemoveAsync())throw new InvalidOperationException(session.Status);}
    }
    session.Observe(state);
    var saveOnlyLibrary=new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(Path.GetDirectoryName(reportPath)!,"save-only.json")),session);
    var saveOnly=await GraphicsStarterWorkflow.AddAsync(session,saveOnlyLibrary,GraphicsShowStarters.Prepare(GraphicsShowStarters.All[0],state!),false,true);
    if(!saveOnly.Saved || saveOnly.StudioAssigned || saveOnlyLibrary.Document.StudioLowerThird is not null || state!.Instances.Any(i=>i.ProgramRevision!=0))throw new InvalidOperationException("Save graphics only assigned or showed a look.");
    checks.Add("Save graphics only remains explicit and does not assign Studio or put anything on air");
    await File.WriteAllTextAsync(reportPath,JsonSerializer.Serialize(new {success=true,nativeExecutable=start.FileName,nativeSha256=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(await File.ReadAllBytesAsync(start.FileName))).ToLowerInvariant(),customBranding=true,customLogo=logo is not null,checks,physicalDisplayVerified=false,outputPerformanceVerified=false},new JsonSerializerOptions {WriteIndented=true}));
    Console.WriteLine($"PASS: {checks.Count} native starter workflow checks; no camera or output enabled.");
} catch(Exception ex) {
    await File.WriteAllTextAsync(reportPath,JsonSerializer.Serialize(new {success=false,error=ex.Message,state},new JsonSerializerOptions(json) {WriteIndented=true}));
    throw;
} finally {
    Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT",previousAssetRoot);
    core.StandardInput.Close();
    if(!core.WaitForExit(5000))core.Kill(entireProcessTree:true);
    await reader;await errors;
}
