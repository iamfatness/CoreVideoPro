// Usage: outputs-drill-prefs-probe <installed app folder> <preferences.json>...
// Prints one JSON line per file: {"file":..,"loaded":bool,...}. Exit 0 always;
// the caller asserts. Read-only: it never writes any file.
using System;
using System.Collections;
using System.IO;
using System.Runtime.Loader;
using System.Text.Json;

var root = args[0];
AssemblyLoadContext.Default.Resolving += (context, name) =>
{
    var candidate = Path.Combine(root, name.Name + ".dll");
    return File.Exists(candidate) ? context.LoadFromAssemblyPath(candidate) : null;
};
var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(root, "CoreVideoPro.WinUI.dll"));
var serializer = assembly.GetType("CoreVideoPro.WinUI.Services.ProductionOutputPreferencesSerializer", throwOnError: true);
var deserialize = serializer.GetMethod("Deserialize", new[] { typeof(string) });
for (var i = 1; i < args.Length; i++)
{
    var value = deserialize.Invoke(null, new object[] { File.ReadAllText(args[i]) });
    object Get(string name) => value?.GetType().GetProperty(name)?.GetValue(value);
    Console.WriteLine(JsonSerializer.Serialize(new
    {
        file = Path.GetFileName(args[i]),
        loaded = value is not null,
        recordingTargetFolder = Get("RecordingTargetFolder")?.ToString(),
        streamRtmpEnabled = Get("StreamRtmpEnabled") as bool?,
        streamSrtPort = Get("StreamSrtPort")?.ToString(),
        streamSrtHost = Get("StreamSrtHost")?.ToString(),
        isoSourceCount = (Get("IsoRecordingSourceIds") as ICollection)?.Count,
    }));
}
