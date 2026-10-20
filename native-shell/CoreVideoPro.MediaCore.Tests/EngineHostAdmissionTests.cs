using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class EngineHostAdmissionTests
{
    [Theory]
    [InlineData("stop-recording-session", true)]
    [InlineData("stop-program-output", true)]
    [InlineData("zoom-cancel", true)]
    [InlineData("load-scene-graph", false)]
    [InlineData("snapshot", false)]
    public void ReservedClassificationIsExplicit(string type, bool expected) =>
        Assert.Equal(expected, EngineHostWire.IsUrgent(JsonSerializer.SerializeToElement(new { type })));

    [Fact]
    public void MixedBatchCannotConsumeReservedCapacity()
    {
        Assert.False(EngineHostWire.IsUrgent(JsonSerializer.SerializeToElement(new {
            type = "media-core-sync", commands = new[] { new { type = "stop-recording-session" }, new { type = "load-scene-graph" } }
        })));
        Assert.True(EngineHostWire.IsUrgent(JsonSerializer.SerializeToElement(new {
            type = "media-core-sync", commands = new[] { new { type = "stop-recording-session" }, new { type = "stop-program-output" } }
        })));
    }

    [WindowsEngineHostFact]
    public async Task UrgentStopBypassesDelayedNativeReplyAndDoesNotRollbackRevision()
    {
        await using var rig = await Rig.Start();
        var slow = rig.Client.SendAsync(new() { ["type"] = "delay-ack" }, rig.Token);
        await rig.WaitFor("admitted");
        var clock = Stopwatch.StartNew();
        using var stop = await rig.Client.SendAsync(new() { ["type"] = "stop-recording-session" }, rig.Token);
        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(3), "Urgent stop waited for ordinary ACK");
        Assert.False(slow.IsCompleted);
        using var delayed = await slow;
        using var next = await rig.Client.SendAsync(new() { ["type"] = "increment" }, rig.Token);
        Assert.Equal(3, next.RootElement.GetProperty("mutations").GetInt32());
    }

    [WindowsEngineHostFact]
    public async Task FourHungOrdinaryClientsCannotBlockStopOrTakeover()
    {
        await using var rig = await Rig.Start();
        var pipes = new List<NamedPipeClientStream>();
        try
        {
            for (var i = 0; i < 4; i++)
            {
                var pipe = await rig.Connect(false);
                pipes.Add(pipe);
                await pipe.WriteAsync(new byte[] { 1 }, rig.Token); // partial length header, never completes
            }
            var clock = Stopwatch.StartNew();
            using var stop = await rig.Client.SendAsync(new() { ["type"] = "stop-program-output" }, rig.Token);
            Assert.True(clock.Elapsed < TimeSpan.FromSeconds(3));
            await using var replacement = new EngineHostClient(rig.Options);
            await replacement.StartAsync(3, rig.Token);
            await replacement.ControlAsync("take-control", rig.Token);
            await Assert.ThrowsAsync<InvalidOperationException>(() => rig.Client.SendAsync(new() { ["type"] = "stop-program-output" }, rig.Token));
            using var after = await replacement.SendAsync(new() { ["type"] = "stop-program-output" }, rig.Token);
            Assert.Equal(2, after.RootElement.GetProperty("mutations").GetInt32());
        }
        finally { foreach (var pipe in pipes) pipe.Dispose(); }
    }

    [WindowsEngineHostFact]
    public async Task BlockedReaderReleasesOrdinaryLaneAndReservedPartialFramesExpire()
    {
        await using var rig = await Rig.Start();
        using var blocked = await rig.Connect(false);
        // A full-size response exceeds the pipe buffer. Do not read any of it.
        await EngineHostWire.WriteAsync(blocked, new EngineHostRequest("command", rig.Id, rig.Epoch,
            Payload: JsonSerializer.SerializeToElement(new { type = "snapshot", large = true })), rig.Token);
        await rig.WaitFor("large");
        using var ping = await rig.Client.SendAsync(new() { ["type"] = "ping" }, rig.Token);
        using var heldA = await rig.Connect(true);
        using var heldB = await rig.Connect(true);
        await heldA.WriteAsync(new byte[] { 1 }, rig.Token);
        await heldB.WriteAsync(new byte[] { 1 }, rig.Token);
        // Both reserved listeners must retire an incomplete request within 2 seconds.
        await Task.Delay(2500, rig.Token);
        using var stop = await rig.Client.SendAsync(new() { ["type"] = "stop-recording-session" }, rig.Token);
        Assert.True(stop.RootElement.GetProperty("ok").GetBoolean());
        using var ordinary = await rig.Client.SendAsync(new() { ["type"] = "ping" }, rig.Token);
    }

    [WindowsEngineHostFact]
    public async Task ReservedEndpointRejectsOrdinaryFloodAndWrongServerIdentity()
    {
        await using var rig = await Rig.Start();
        for (var i = 0; i < 8; i++)
        {
            using var pipe = await rig.Connect(true);
            await EngineHostWire.WriteAsync(pipe, new EngineHostRequest("command", rig.Id, rig.Epoch,
                Payload: JsonSerializer.SerializeToElement(new { type = "snapshot" })), rig.Token);
            await Assert.ThrowsAnyAsync<IOException>(() => EngineHostWire.ReadAsync<EngineHostReply>(pipe, rig.Token));
        }
        using var stop = await rig.Client.SendAsync(new() { ["type"] = "stop-program-output" }, rig.Token);
        using var identity = await rig.Connect(true);
        Assert.Throws<IOException>(() => EngineHostEndpoint.VerifyServer(identity, Environment.ProcessPath!));
    }

    [WindowsEngineHostFact]
    public async Task SynchronousUiDetachDoesNotRequireTheBlockedDispatcher()
    {
        await using var rig = await Rig.Start();
        Exception? failure = null;
        var context = new NonPumpingContext();
        var thread = new Thread(() =>
        {
            SynchronizationContext.SetSynchronizationContext(context);
            try { rig.Client.DisposeAsync().AsTask().GetAwaiter().GetResult(); }
            catch (Exception ex) { failure = ex; }
        }) { IsBackground = true };
        thread.Start();
        Assert.True(thread.Join(4000), "Detach deadlocked on the UI synchronization context");
        Assert.Null(failure);
        Assert.Equal(0, context.Posts);
    }

    [WindowsEngineHostFact]
    public async Task LostAckAfterDisconnectReturnsRetainedOutcomeWithoutRepeatingMutation()
    {
        await using var rig = await Rig.Start();
        async Task<EngineHostReply> Exchange(EngineHostRequest request, bool reserved)
        {
            using var pipe = await rig.Connect(reserved);
            await EngineHostWire.WriteAsync(pipe, request, rig.Token);
            return await EngineHostWire.ReadAsync<EngineHostReply>(pipe, rig.Token);
        }
        var attached = await Exchange(new("attach", rig.Id), true);
        var writer = await Exchange(new("take-control", rig.Id, attached.Epoch, attached.Fence, attached.Revision), true);
        var command = new EngineHostRequest("command", rig.Id, writer.Epoch, writer.Fence, writer.Revision,
            "1", JsonSerializer.SerializeToElement(new { type = "increment" }));
        using (var lost = await rig.Connect(false))
        {
            await EngineHostWire.WriteAsync(lost, command, rig.Token);
            await rig.WaitFor("increment");
            // Disconnect before reading the reply; admission cannot be cancelled.
        }
        var recovered = await Exchange(command, false);
        Assert.True(recovered.Ok);
        Assert.Equal(1, recovered.Payload!.Value.GetProperty("mutations").GetInt32());
        var observed = await Exchange(new("command", rig.Id, writer.Epoch,
            Payload: JsonSerializer.SerializeToElement(new { type = "ping" })), false);
        Assert.Equal(1, observed.Payload!.Value.GetProperty("mutations").GetInt32());
    }

    [Fact]
    public void ParticipantResourcesUseNestedIdentity()
    {
        string? Key(string id) => EngineHostWire.ResourceKey(JsonSerializer.SerializeToElement(new {
            type = "participant-shared-texture", texture = new { participantId = id }
        }));
        Assert.Equal("participant-shared-texture:a", Key("a"));
        Assert.NotEqual(Key("a"), Key("b"));
    }

    [WindowsEngineHostFact]
    public async Task StableResourceDescriptorsArePublishedOncePerClient()
    {
        await using var rig = await Rig.Start();
        await using var observer = new EngineHostClient(rig.Options);
        var events = 0;
        observer.Resource += _ => Interlocked.Increment(ref events);
        await observer.StartAsync(3, rig.Token);
        for (var i = 0; i < 5; i++)
        {
            using var ping = await observer.SendAsync(new() { ["type"] = "ping" }, rig.Token);
        }
        Assert.Equal(1, events);
    }

    private sealed class NonPumpingContext : SynchronizationContext
    {
        public int Posts;
        public override void Post(SendOrPostCallback callback, object? state) => Interlocked.Increment(ref Posts);
    }

    private sealed class Rig : IAsyncDisposable
    {
        public required EngineHostClientOptions Options;
        public required EngineHostClient Client;
        public required string Directory;
        public string Id = Guid.NewGuid().ToString("N");
        public string? Epoch;
        private readonly CancellationTokenSource _deadline = new(TimeSpan.FromSeconds(40));
        public CancellationToken Token => _deadline.Token;
        public static async Task<Rig> Start()
        {
            var directory = Path.Combine(Path.GetTempPath(), "cv-host-admission-" + Guid.NewGuid().ToString("N"));
            System.IO.Directory.CreateDirectory(directory);
            var fixture = Path.Combine(directory, "core.cjs");
            await File.WriteAllTextAsync(fixture, """
                const fs=require('node:fs'), path=require('node:path'); let mutations=0;
                const profile={name:'admission',renderer:'software',maxProgramResolution:'1920x1080'};
                console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile}));
                require('node:readline').createInterface({input:process.stdin}).on('line',line=>{
                  const m=JSON.parse(line); if(!['handshake','snapshot','ping'].includes(m.type)) mutations++;
                  console.log(JSON.stringify({type:'program-shared-texture',texture:{sharedHandleHex:'0x1234',width:1920,height:1080,frameNumber:1}}));
                  const reply={id:m.id,ok:true,type:m.type,profile,mutations};
                  if(m.type==='increment') fs.writeFileSync(path.join(__dirname,'increment'),'1');
                  if(m.type==='delay-ack') { fs.writeFileSync(path.join(__dirname,'admitted'),'1'); setTimeout(()=>console.log(JSON.stringify(reply)),5000); return; }
                  if(m.large) { reply.padding='x'.repeat(2*1024*1024); fs.writeFileSync(path.join(__dirname,'large'),'1'); }
                  console.log(JSON.stringify(reply));
                }).on('close',()=>process.exit(0));
                """);
            var node = (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)
                .Select(p => Path.Combine(p, "node.exe")).First(File.Exists);
            var executable = Path.Combine(AppContext.BaseDirectory.Replace("CoreVideoPro.MediaCore.Tests", "CoreVideoPro.EngineHost"), "CoreVideoPro.EngineHost.exe");
            var options = new EngineHostClientOptions(executable, node, Guid.NewGuid().ToString("N"), [fixture]);
            var rig = new Rig { Options = options, Client = new(options), Directory = directory };
            await rig.Client.StartAsync(3, rig.Token);
            using var pipe = await rig.Connect(true);
            await EngineHostWire.WriteAsync(pipe, new EngineHostRequest("attach", rig.Id), rig.Token);
            rig.Epoch = (await EngineHostWire.ReadAsync<EngineHostReply>(pipe, rig.Token)).Epoch;
            return rig;
        }
        public async Task<NamedPipeClientStream> Connect(bool reserved)
        {
            var pipe = new NamedPipeClientStream(".", EngineHostEndpoint.Name(Options.Profile) + (reserved ? ".urgent" : ""), PipeDirection.InOut, PipeOptions.Asynchronous);
            await pipe.ConnectAsync(4000, Token);
            return pipe;
        }
        public async Task WaitFor(string marker)
        {
            while (!File.Exists(Path.Combine(Directory, marker))) await Task.Delay(10, Token);
        }
        public async ValueTask DisposeAsync()
        {
            await using var cleanup = new EngineHostClient(Options);
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
            await cleanup.StartAsync(3, deadline.Token);
            await cleanup.ControlAsync("take-control", deadline.Token);
            await cleanup.ControlAsync("shutdown", deadline.Token);
            await Client.DisposeAsync();
            _deadline.Dispose();
        }
    }
}
