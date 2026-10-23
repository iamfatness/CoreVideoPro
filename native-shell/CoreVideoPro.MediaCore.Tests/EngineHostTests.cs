using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public class EngineHostTests
{
    [Fact]
    public void LeaseHandoverFencesOldWriterAndReattachIsReadOnly()
    {
        var session = new EngineHostSession();
        Assert.False(session.Attach("first"));
        var old = new EngineHostRequest("command", "first", session.Epoch, session.Fence, session.Revision);
        Assert.Null(session.Validate(old));
        Assert.True(session.Attach("second"));
        session.TakeControl("second");
        Assert.Equal("writer-fenced", session.Validate(old));
        session.Detach("first");
        Assert.Equal("second", session.Writer);
    }

    [Fact]
    public void RevisionAndEpochPreventStaleMutation()
    {
        var session = new EngineHostSession();
        session.Attach("a");
        var command = new EngineHostRequest("command", "a", session.Epoch, session.Fence, session.Revision);
        session.ReserveMutation();
        Assert.Equal("revision-conflict", session.Validate(command));
        Assert.Equal("host-epoch-changed", session.Validate(command with { Epoch = "old" }));
    }

    [Fact]
    public void OutcomesAreBoundedAndSameIdCannotChangeMeaning()
    {
        var session = new EngineHostSession();
        var result = JsonSerializer.SerializeToElement(new { ok = true });
        for (var i = 1; i <= 140; i++) session.Commit("a", i.ToString(), "same", result);
        Assert.False(session.TryResult("a", "1", "same", out _));
        Assert.True(session.TryResult("a", "140", "same", out _));
        Assert.Throws<InvalidOperationException>(() => session.TryResult("a", "140", "changed", out _));
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(0)]
    [InlineData(8388609)]
    public async Task RejectsOversizedFrameBeforeAllocation(int length)
    {
        using var stream = new MemoryStream(BitConverter.GetBytes(length));
        await Assert.ThrowsAsync<IOException>(() => EngineHostWire.ReadAsync<EngineHostRequest>(stream, default));
    }

    [Fact]
    public void ObservationClassificationNeverAdmitsSyncActions()
    {
        Assert.True(EngineHostWire.IsObservation(JsonSerializer.SerializeToElement(new { type = "media-core-sync", commands = Array.Empty<object>() })));
        Assert.False(EngineHostWire.IsObservation(JsonSerializer.SerializeToElement(new { type = "media-core-sync", commands = new[] { new { type = "take" } } })));
        Assert.False(EngineHostWire.IsObservation(JsonSerializer.SerializeToElement(new { type = "new-command" })));
    }

    [WindowsEngineHostFact]
    public async Task RealHostRetainsChildAcrossDetachAndRejectsStaleClient()
    {
        var executable = Path.Combine(AppContext.BaseDirectory.Replace("CoreVideoPro.MediaCore.Tests", "CoreVideoPro.EngineHost"), "CoreVideoPro.EngineHost.exe");
        Assert.True(File.Exists(executable), "Build EngineHost Release before integration tests.");
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-host-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var fixture = Path.Combine(directory, "core.cjs");
        await File.WriteAllTextAsync(fixture, """
            const readline = require('node:readline');
            const profile = {name:'host-test',renderer:'software',maxProgramResolution:'1920x1080'};
            let mutations=0;
            console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile}));
            readline.createInterface({input:process.stdin}).on('line', line => {
              const m=JSON.parse(line); if(m.type==='increment') mutations++;
              console.log(JSON.stringify({id:m.id,ok:true,type:m.type,profile,pid:process.pid,mutations,echo:m.payload}));
            }).on('close',()=>process.exit(0));
            """);
        var node = Environment.GetEnvironmentVariable("NODE_EXE") ??
            (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator)
                .Select(path => Path.Combine(path, "node.exe")).First(File.Exists);
        var options = new EngineHostClientOptions(executable, node, Guid.NewGuid().ToString("N"), [fixture]);
        await using var clientA = new EngineHostClient(options);
        await using var clientB = new EngineHostClient(options);
        var first = clientA;
        var second = clientB;
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(45));
        try
        {
            await Task.WhenAll(clientA.StartAsync(3, deadline.Token), clientB.StartAsync(3, deadline.Token));
            Assert.NotEqual(clientA.ReadOnly, clientB.ReadOnly);
            if (first.ReadOnly) (first, second) = (second, first);
            var pid = first.CorePid;
            using var mutation = await first.SendAsync(new() { ["type"] = "increment", ["payload"] = new { FieldName = "wire-parity" } }, deadline.Token);
            Assert.Equal(1, mutation.RootElement.GetProperty("mutations").GetInt32());
            Assert.Equal("wire-parity", mutation.RootElement.GetProperty("echo").GetProperty("fieldName").GetString());
            await second.StartAsync(3, deadline.Token);
            Assert.True(second.ReadOnly);
            await Assert.ThrowsAsync<InvalidOperationException>(() => second.SendAsync(new() { ["type"] = "increment" }, deadline.Token));
            Assert.Equal(pid, second.CorePid);
            await first.DisposeAsync();
            using var observation = await second.SendAsync(new() { ["type"] = "ping" }, deadline.Token);
            Assert.Equal(1, observation.RootElement.GetProperty("mutations").GetInt32());
            Assert.Equal(pid, observation.RootElement.GetProperty("pid").GetInt32());
            await second.ControlAsync("take-control", deadline.Token);
            using var next = await second.SendAsync(new() { ["type"] = "increment" }, deadline.Token);
            Assert.Equal(2, next.RootElement.GetProperty("mutations").GetInt32());

            async Task<EngineHostReply> Raw(EngineHostRequest request)
            {
                using var pipe = new NamedPipeClientStream(".", EngineHostEndpoint.Name(options.Profile), PipeDirection.InOut, PipeOptions.Asynchronous);
                await pipe.ConnectAsync(2000, deadline.Token);
                await EngineHostWire.WriteAsync(pipe, request, deadline.Token);
                return await EngineHostWire.ReadAsync<EngineHostReply>(pipe, deadline.Token);
            }
            var client = Guid.NewGuid().ToString("N");
            var attached = await Raw(new("attach", client));
            Assert.True(attached.ReadOnly);
            var claimed = await Raw(new("take-control", client, attached.Epoch, attached.Fence, attached.Revision));
            await Assert.ThrowsAsync<InvalidOperationException>(() => second.SendAsync(new() { ["type"] = "increment" }, deadline.Token));
            var command = new EngineHostRequest("command", client, claimed.Epoch, claimed.Fence, claimed.Revision,
                "1", JsonSerializer.SerializeToElement(new { type = "increment" }));
            var lostAck = await Raw(command);
            var replay = await Raw(command);
            Assert.Equal(3, replay.Payload!.Value.GetProperty("mutations").GetInt32());
            Assert.Equal(lostAck.Revision, replay.Revision);
            var prior = replay;
            for (var i = 2; i <= 131; i++)
                prior = await Raw(command with { Revision = prior.Revision, CommandId = i.ToString() });
            var expired = await Raw(command);
            Assert.Equal("command-outcome-unknown", expired.Error);
            var incompatible = await Raw(new("attach", client, Version: 2));
            Assert.Equal("incompatible-host-protocol", incompatible.Error);
            for (var i = 0; i < 70; i++) Assert.True((await Raw(new("attach", Guid.NewGuid().ToString("N")))).Ok);
            // Observer churn must not brick a long-running show or revoke its writer.
            var currentWriter = await Raw(command with { Revision = prior.Revision, CommandId = "132" });
            Assert.True(currentWriter.Ok);
        }
        finally
        {
            if (first.CorePid != 0)
            {
                await using var cleanup = new EngineHostClient(options);
                await cleanup.StartAsync(3, CancellationToken.None);
                await cleanup.ControlAsync("take-control", CancellationToken.None);
                await cleanup.ControlAsync("shutdown", CancellationToken.None);
            }
        }
    }

}

public sealed class WindowsEngineHostFactAttribute : FactAttribute
{
    public WindowsEngineHostFactAttribute()
    {
        if (!OperatingSystem.IsWindows()) Skip = "Windows local-pipe/process evidence is unavailable on this platform.";
    }
}
