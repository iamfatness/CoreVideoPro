using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// #725: a core the supervisor respawned is a NEW roster authority. Its epoch restarts at the
/// same process and meeting numbers as the dead one (only the diagnostic token differs), so it
/// does not order after the installed barrier. Live 2026-10-01: the shell kept the dead core's
/// roster for the rest of the session and every Take flipped the multiview between the two.
/// </summary>
public sealed class RosterAfterCoreRespawnTests
{
    [Fact]
    public async Task ARespawnedCoresRosterReplacesTheDeadCoresRoster()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-roster-respawn-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            // The first process serves the old meeting roster at a HIGH revision and dies on
            // command. Its replacement serves a different roster at a LOWER revision under an
            // epoch with the same process and meeting numbers: the measured live shape
            // (1:1:18772-0 rev 38, then 1:1:23540-0 rev 11).
            await File.WriteAllTextAsync(script, """
                const readline = require('node:readline');
                const fs = require('node:fs');
                const path = require('node:path');
                const marker = path.join(__dirname, 'second-generation');
                const second = fs.existsSync(marker);
                fs.writeFileSync(marker, '1');
                const profile = {name:'roster-respawn-fake',renderer:'software',maxProgramResolution:'1920x1080'};
                const roster = second
                  ? {meetingState:'in_meeting', rosterEpoch:'1:1:core-b', rosterRevision:11,
                     participants:[{userId:'200',displayName:'Producer after',sourceGeneration:1,muted:false,videoOn:false}]}
                  : {meetingState:'in_meeting', rosterEpoch:'1:1:core-a', rosterRevision:38,
                     participants:[{userId:'100',displayName:'Producer before',sourceGeneration:1,muted:false,videoOn:false}]};
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const m = JSON.parse(line);
                  let response = {id:m.id,ok:true,type:m.type};
                  if (m.type === 'handshake') response = {...response,protocolVersion:{major:1,minor:0},profile};
                  if (m.type === 'media-core-sync') {
                    if (m.commands?.some(c => c.type === 'test-die')) process.exit(9);
                    response.state = {health:{frameCount:1},profile,meetingState:'in_meeting',programFrameCount:1};
                  }
                  if (m.type === 'zoom-snapshot') response.snapshot = roster;
                  console.log(JSON.stringify(response));
                });
                """);
            var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = directory,
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 3000,
                FrameDrainIntervalMs = 100000
            });
            await using var bridge = new MediaCoreBridgeService(supervisor);
            Assert.NotNull(await bridge.StartAsync());
            await bridge.GetZoomSnapshotAsync();
            Assert.Equal("100", bridge.LastSnapshot!.Participants[0].UserId);

            try { await bridge.SyncAsync([new NativeMediaCoreCommand { Type = "test-die" }]); }
            catch (Exception) { /* the core died mid-request, as a crash does */ }
            await UntilAsync(() => supervisor.Health.RestartCount >= 1 && !supervisor.Health.Recovering);

            await UntilAsync(async () =>
            {
                try { await bridge.GetZoomSnapshotAsync(); return true; }
                catch (Exception) { return false; }
            });

            Assert.Equal("1:1:core-b", bridge.LastSnapshot!.RosterEpoch);
            Assert.Equal("200", Assert.Single(bridge.LastSnapshot.Participants).UserId);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); }
            catch (IOException) { }
        }
    }

    private static Task UntilAsync(Func<bool> condition) => UntilAsync(() => Task.FromResult(condition()));

    private static async Task UntilAsync(Func<Task<bool>> condition)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            if (await condition()) return;
            await Task.Delay(50);
        }
        Assert.True(await condition());
    }
}
