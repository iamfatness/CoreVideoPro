using CoreVideoPro.MediaCore.Services;

if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("EngineHost requires Windows.");
string Required(string name) => Array.IndexOf(args, name) is var i && i >= 0 && i + 1 < args.Length
    ? args[i + 1] : throw new ArgumentException($"Missing {name}");
var profile = Required("--profile");
if (Environment.GetEnvironmentVariable("COREVIDEO_ENGINE_HOST_LOG_DIR") is { Length: > 0 } logDirectory)
    DiagnosticLog.ConfigureVerificationDirectory(logDirectory);
var core = Path.GetFullPath(Required("--core"));
if (!File.Exists(core)) throw new FileNotFoundException("Native core unavailable", core);
var endpoint = EngineHostEndpoint.Name(profile);
// Keep ownership on this thread: Mutex ownership is thread-affine across awaits.
using var singleton = new Mutex(true, "Local\\" + endpoint, out var created);
if (!created) return;
using var stop = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; stop.Cancel(); };
try
{
    Run().GetAwaiter().GetResult();
}
finally { singleton.ReleaseMutex(); }

async Task Run()
{
    var coreArgs = Enumerable.Range(0, args.Length - 1).Where(i => args[i] == "--core-arg").Select(i => args[i + 1]).ToArray();
    await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions { Command = core, Args = coreArgs, PublishHostResources = true });
    if (Array.IndexOf(args, "--buffer") is var index && index >= 0 && index + 1 < args.Length)
        supervisor.ConfigureProgramBufferFrames(int.Parse(args[index + 1]));
    await supervisor.StartAsync(stop.Token);
    var server = new EngineHostServer(supervisor, endpoint);
    await server.RunAsync(stop.Token);
    supervisor.StopForAppExit(TimeSpan.FromSeconds(10));
}
