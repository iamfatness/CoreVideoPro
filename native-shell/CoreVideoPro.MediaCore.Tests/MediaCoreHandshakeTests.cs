using System.Text.Json;
using System.Reflection;
using System.Diagnostics;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreHandshakeTests
{
    [Fact]
    public async Task IncompatibleRequestOnlyChildIsTerminallyRejected()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-rejected-request-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "request-only.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            await File.WriteAllTextAsync(script, """
                const fs = require('node:fs'), readline = require('node:readline');
                fs.appendFileSync(process.env.COREVIDEO_HANDSHAKE_TRACE, 'started\n');
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const message = JSON.parse(line);
                  fs.appendFileSync(process.env.COREVIDEO_HANDSHAKE_TRACE, message.type + '\n');
                  console.log(JSON.stringify({id:message.id,ok:true,type:'handshake',protocolVersion:{major:2,minor:0},profile:{name:'incompatible',renderer:'software',maxProgramResolution:'1920x1080'}}));
                });
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_HANDSHAKE_TRACE"] = trace },
                HandshakeRequestTimeoutMs = 1000, RequestTimeoutMs = 3000, FrameDrainIntervalMs = 100000, MaxRestarts = 3
            });
            var startup = supervisor.StartAsync();
            var process = (Process)typeof(MediaCoreSupervisor).GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            using var child = Process.GetProcessById(process.Id);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => startup.WaitAsync(TimeSpan.FromSeconds(5)))).Message);
            await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(supervisor.Running);
            Assert.True(supervisor.Health.Stopped);
            Assert.False(supervisor.Health.Recovering);
            Assert.Equal(0, supervisor.Health.RestartCount);
            Assert.Null(supervisor.Profile);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.StartAsync())).Message);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.HandshakeAsync())).Message);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.PingAsync())).Message);
            Assert.Equal("started\nhandshake\n", await File.ReadAllTextAsync(trace));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task RequestOnlyChildCanHandshakeWhileOrdinaryCommandsRemainGated()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-request-handshake-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "request-only.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            await File.WriteAllTextAsync(script, """
                const fs = require('node:fs'), readline = require('node:readline');
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const message = JSON.parse(line);
                  fs.appendFileSync(process.env.COREVIDEO_HANDSHAKE_TRACE, message.type + '\n');
                  const response = {id:message.id,ok:true,type:message.type};
                  if(message.type === 'handshake') {
                    response.protocolVersion = {major:1,minor:0};
                    response.profile = {name:'request-only',renderer:'software',maxProgramResolution:'1920x1080'};
                  }
                  console.log(JSON.stringify(response));
                });
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_HANDSHAKE_TRACE"] = trace },
                HandshakeRequestTimeoutMs = 1000, RequestTimeoutMs = 3000, FrameDrainIntervalMs = 100000
            });
            var startup = supervisor.StartAsync();
            var blocked = await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.PingAsync());
            Assert.Contains("handshake", blocked.Message, StringComparison.OrdinalIgnoreCase);
            Assert.Equal("request-only", (await startup.WaitAsync(TimeSpan.FromSeconds(5)))?.Name);
            Assert.True(await supervisor.PingAsync());
            Assert.Equal("handshake\nping\n", await File.ReadAllTextAsync(trace));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task QueuedCommandCannotCrossIntoReplacementProcess()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-generation-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "generation.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            var exitSignal = Path.Combine(directory, "exit-first");
            await File.WriteAllTextAsync(script, """
                const fs = require('node:fs'), readline = require('node:readline');
                const trace = process.env.COREVIDEO_GENERATION_TRACE;
                const first = !fs.existsSync(trace);
                fs.appendFileSync(trace, 'started\n');
                console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile:{name:'compatible',renderer:'software',maxProgramResolution:'1920x1080'}}));
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const message = JSON.parse(line);
                  fs.appendFileSync(trace, 'received:' + message.id + '\n');
                  console.log(JSON.stringify({id:message.id,ok:true}));
                });
                setInterval(() => { if(first && fs.existsSync(process.env.COREVIDEO_EXIT_SIGNAL)) process.exit(23); }, 10);
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                // Keep the fixture directory out of inherited process/console-host cwd handles.
                // Script, trace and control-file paths are absolute.
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_GENERATION_TRACE"] = trace, ["COREVIDEO_EXIT_SIGNAL"] = exitSignal },
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 5000, FrameDrainIntervalMs = 100000, MaxRestarts = 1
            });
            var generation = 0;
            var replacementReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            supervisor.ProfileChanged += _ => { if (Interlocked.Increment(ref generation) == 2) replacementReady.TrySetResult(); };
            await supervisor.StartAsync();
            // Hold the actual write gate to deterministically reproduce an old
            // command waiting while its original process is replaced.
            var gate = (SemaphoreSlim)typeof(MediaCoreSupervisor).GetField("_stdinGate", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(supervisor)!;
            await gate.WaitAsync();
            Task<bool> staleRequest;
            try
            {
                staleRequest = supervisor.PingAsync();
                await File.WriteAllTextAsync(exitSignal, "exit");
                await replacementReady.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            finally { gate.Release(); }
            await Assert.ThrowsAsync<InvalidOperationException>(() => staleRequest);
            Assert.True(await supervisor.PingAsync());
            var observed = await File.ReadAllTextAsync(trace);
            Assert.DoesNotContain("received:core-1", observed);
            Assert.Contains("received:core-2", observed);
            Assert.Equal(1, supervisor.Health.RestartCount);
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task CompatibleChildCanStopWithoutWaitingOnExitHandlerGate()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-compatible-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "compatible.cjs");
            await File.WriteAllTextAsync(script, """
                console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile:{name:'compatible',renderer:'software',maxProgramResolution:'1920x1080'}}));
                setInterval(() => {}, 1000);
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                // Keep the fixture directory out of inherited process/console-host cwd handles.
                // Script, trace and control-file paths are absolute.
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                HandshakeRequestTimeoutMs = 3000, FrameDrainIntervalMs = 10000
            });
            Assert.NotNull(await supervisor.StartAsync());
            Assert.True(supervisor.Running);
            await Task.Run(supervisor.Stop).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(supervisor.Running);
            Assert.Equal(0, supervisor.Health.RestartCount);
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task RejectedHandshakeStopsTransportAndCannotBeBypassedBySecondStart()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-handshake-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        Process? rejectedChild = null;
        try
        {
            var script = Path.Combine(directory, "incompatible.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            var releaseHandshake = Path.Combine(directory, "release-handshake");
            await File.WriteAllTextAsync(script, """
                const fs = require('node:fs');
                fs.appendFileSync(process.env.COREVIDEO_HANDSHAKE_TRACE, 'started\n');
                process.stdin.on('data', data => fs.appendFileSync(process.env.COREVIDEO_HANDSHAKE_TRACE, 'request\n'));
                const handshake = setInterval(() => {
                  if (!fs.existsSync(process.env.COREVIDEO_RELEASE_HANDSHAKE)) return;
                  clearInterval(handshake);
                  console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:2,minor:0},profile:{name:'incompatible'}}));
                }, 10);
                setInterval(() => {}, 1000);
                """);
            await using var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                // Keep the fixture directory out of inherited process/console-host cwd handles.
                // Script, trace and control-file paths are absolute.
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string>
                {
                    ["COREVIDEO_HANDSHAKE_TRACE"] = trace,
                    ["COREVIDEO_RELEASE_HANDSHAKE"] = releaseHandshake
                },
                HandshakeRequestTimeoutMs = 3000, RequestTimeoutMs = 1000, MaxRestarts = 5
            });
            var firstStart = supervisor.StartAsync();
            // Hold an independent OS process handle before allowing rejection.
            // The supervisor invalidates its transport before asynchronous
            // stdout processing has finished killing/disposing this child.
            var child = (Process)typeof(MediaCoreSupervisor).GetField("_process", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(supervisor)!;
            rejectedChild = Process.GetProcessById(child.Id);
            _ = rejectedChild.Handle;
            // A concurrent Start must await validation instead of returning a null
            // profile through the already-running process shortcut.
            var secondStart = supervisor.StartAsync();
            var pendingHandshake = await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.PingAsync());
            Assert.Contains("handshake", pendingHandshake.Message, StringComparison.OrdinalIgnoreCase);
            await File.WriteAllTextAsync(releaseHandshake, "release");
            var firstFailure = await Assert.ThrowsAsync<InvalidOperationException>(() => firstStart);
            var secondFailure = await Assert.ThrowsAsync<InvalidOperationException>(() => secondStart);
            Assert.Contains("incompatible", firstFailure.Message);
            Assert.Contains("incompatible", secondFailure.Message);
            Assert.False(supervisor.Running);
            Assert.True(supervisor.Health.Stopped);
            Assert.False(supervisor.Health.Recovering);
            Assert.Equal(0, supervisor.Health.RestartCount);
            Assert.Null(supervisor.Profile);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.StartAsync())).Message);
            Assert.Contains("incompatible", (await Assert.ThrowsAsync<InvalidOperationException>(() => supervisor.PingAsync())).Message);
            Assert.Equal("started\n", await File.ReadAllTextAsync(trace));
        }
        finally
        {
            if (rejectedChild is not null)
            {
                using (rejectedChild)
                    await rejectedChild.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
            }
            Directory.Delete(directory, recursive: true);
        }
    }

    [Theory]
    [InlineData("{}", true)]
    [InlineData("{\"protocolVersion\":{\"major\":1,\"minor\":0}}", true)]
    [InlineData("{\"protocolVersion\":{\"major\":1,\"minor\":100}}", true)]
    [InlineData("{\"protocolVersion\":{\"major\":2,\"minor\":0}}", false)]
    [InlineData("{\"protocolVersion\":null}", false)]
    public void ExplicitProtocolVersionMustBeCompatible(string json, bool compatible)
    {
        using var doc = JsonDocument.Parse(json);
        if (compatible) MediaCoreHandshakeRules.RequireCompatibleProtocol(doc.RootElement);
        else Assert.Throws<InvalidOperationException>(() => MediaCoreHandshakeRules.RequireCompatibleProtocol(doc.RootElement));
    }
    [Fact]
    public void IsUnsolicitedBootstrapHandshake_AcceptsOnlyStartupLine()
    {
        using var bootstrap = JsonDocument.Parse(
            """{"id":"handshake","ok":true,"type":"handshake","profile":{"name":"CoreVideo Pro Native Media Core"}}""");
        using var explicitHandshake = JsonDocument.Parse(
            """{"id":"core-1","ok":true,"type":"handshake","profile":{"name":"CoreVideo Pro Native Media Core"}}""");

        Assert.True(MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake(bootstrap.RootElement));
        Assert.False(MediaCoreHandshakeRules.IsUnsolicitedBootstrapHandshake(explicitHandshake.RootElement));
    }
}
