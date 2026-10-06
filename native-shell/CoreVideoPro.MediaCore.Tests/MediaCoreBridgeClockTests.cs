using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCoreBridgeClockTests
{
    private sealed class Clock : TimeProvider
    {
        public long Ticks;
        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => Ticks;
    }

    [Fact]
    public async Task PollsUseMonotonicTimeAndExplicitTestTimeDoesNotChangeTheSessionClock()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-clock-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "clock-core.cjs");
            await File.WriteAllTextAsync(script, """
                const readline = require('node:readline');
                const profile = {name:'clock-fake',renderer:'software',maxProgramResolution:'1920x1080'};
                console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile}));
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const m = JSON.parse(line);
                  let response = {id:m.id, ok:true, type:m.type};
                  if (m.type === 'handshake') response = {...response, protocolVersion:{major:1,minor:0}, profile};
                  if (m.type === 'media-core-sync') response.state = {profile,diagnostics:{generatedAtMs:m.elapsedMs}};
                  console.log(JSON.stringify(response));
                });
                """);
            var time = new Clock { Ticks = 123000 };
            await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = directory,
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 3000, FrameDrainIntervalMs = 100000
            }), time);
            await bridge.StartAsync();
            time.Ticks += 60000;
            Assert.Equal(60000, (await bridge.SyncAsync([])).Diagnostics.GeneratedAtMs);
            Assert.Equal(900000, (await bridge.SyncAsync([], elapsedMs: 900000)).Diagnostics.GeneratedAtMs);
            Assert.Equal(60000, (await bridge.SyncAsync([])).Diagnostics.GeneratedAtMs);
            time.Ticks += 15000;
            Assert.Equal(75000, (await bridge.SyncAsync([])).Diagnostics.GeneratedAtMs);
            bridge.Stop();
            await bridge.StartAsync();
            Assert.Equal(0, (await bridge.SyncAsync([])).Diagnostics.GeneratedAtMs);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); }
            catch (IOException) { }
        }
    }
}
