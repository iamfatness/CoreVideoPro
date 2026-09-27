using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using System.Collections.Concurrent;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class RosterRecoveryBridgeTests
{
    [Fact]
    public async Task GapAndNewEpochWaitForSnapshotWithoutPublishingStaleGuestState()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-roster-recovery-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            await File.WriteAllTextAsync(script, """
                const readline = require('node:readline');
                const profile = {name:'roster-recovery-fake',renderer:'software',maxProgramResolution:'1920x1080'};
                let phase = 0, recoveryReads = 0;
                const roster = (epoch, revision, name, muted) => ({meetingState:'in_meeting',
                  rosterEpoch:epoch, rosterRevision:revision,
                  participants:[{userId:'42',displayName:name,sourceGeneration:1,muted,videoOn:true}]});
                const fact = value => console.log(JSON.stringify({type:'zoom-roster-fact',roster:value}));
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const m = JSON.parse(line);
                  let response = {id:m.id,ok:true,type:m.type};
                  if (m.type === 'handshake') response = {...response,protocolVersion:{major:1,minor:0},profile};
                  if (m.type === 'media-core-sync') {
                    if (m.commands?.some(c => c.type === 'test-gap')) {
                      phase = 1;
                      fact(roster('1:1:engine',3,'Guest',true));
                    }
                    if (m.commands?.some(c => c.type === 'test-rejoin')) {
                      phase = 2;
                      fact(roster('1:2:engine',1,'Rejoined',true));
                    }
                    response.state = {health:{frameCount:1},profile,meetingState:'in_meeting',programFrameCount:1};
                  }
                  if (m.type === 'zoom-snapshot') {
                    response.snapshot = phase === 0 ? roster('1:1:engine',1,'Guest',true) :
                      phase === 1 ? (++recoveryReads === 1
                        ? roster('1:1:engine',2,'Incomplete',true)
                        : roster('1:1:engine',3,'Guest',false)) :
                      roster('1:2:engine',1,'Rejoined',false);
                  }
                  console.log(JSON.stringify(response));
                  if (m.type === 'zoom-snapshot' && phase === 1 && recoveryReads >= 2)
                    setTimeout(() => fact(roster('1:1:engine',2,'Stale',true)), 20);
                  if (m.type === 'zoom-snapshot' && phase === 2)
                    setTimeout(() => fact(roster('1:1:engine',4,'Retired',true)), 20);
                });
                """);
            await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = directory,
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 3000,
                FrameDrainIntervalMs = 100000
            }));
            var seen = new ConcurrentQueue<string>();
            bridge.SnapshotChanged += snapshot =>
            {
                foreach (var participant in snapshot.Participants)
                    seen.Enqueue(participant.DisplayName);
            };
            Assert.NotNull(await bridge.StartAsync());
            await bridge.GetZoomSnapshotAsync();
            Assert.True(bridge.LastSnapshot!.Participants[0].Muted);

            await bridge.SyncAsync([new NativeMediaCoreCommand { Type = "test-gap" }]);
            await UntilAsync(() => bridge.LastSnapshot?.RosterRevision == 3 &&
                bridge.LastSnapshot.Participants[0].Muted == false);
            await UntilAsync(() => bridge.ControlRecovery.RosterStaleSnapshots > 0);
            Assert.Equal("Guest", bridge.LastSnapshot!.Participants[0].DisplayName);
            Assert.DoesNotContain("Incomplete", seen);
            Assert.Equal(1, bridge.ControlRecovery.RosterMissingRevisions);
            Assert.False(bridge.ControlRecovery.RosterReconciling);

            await bridge.SyncAsync([new NativeMediaCoreCommand { Type = "test-rejoin" }]);
            await UntilAsync(() => bridge.LastSnapshot?.RosterEpoch == "1:2:engine" &&
                bridge.LastSnapshot.Participants[0].Muted == false);
            await UntilAsync(() => bridge.ControlRecovery.RosterStaleSnapshots > 1);
            Assert.Equal("Rejoined", bridge.LastSnapshot!.Participants[0].DisplayName);
            Assert.False(bridge.ControlRecovery.RosterReconciling);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); }
            catch (IOException) { }
        }
    }

    private static async Task UntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (!condition() && DateTime.UtcNow < deadline) await Task.Delay(20);
        Assert.True(condition());
    }
}
