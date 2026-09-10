using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// T1.5 (#432): while Engine (Zoom capture) is off in a meeting, the bridge must keep polling
/// the core. It used to poll only the Zoom roster there, so the shell's copy of core state froze
/// (<c>/snapshot</c> aged, the program frame count stopped) until Engine On.
/// </summary>
public sealed class MediaCoreBridgePollTests
{
    [Theory]
    [InlineData(false, "in_meeting", true)]    // Engine off in a meeting: core AND roster
    [InlineData(false, "in-meeting", true)]    // the engine's hyphenated spelling
    [InlineData(true, "in_meeting", false)]    // Engine on: the spine carries the roster
    [InlineData(false, "idle", false)]         // no meeting: nothing to refresh
    [InlineData(false, null, false)]
    public void PollPlan_AlwaysPollsTheCore(bool spineConfigured, string? meetingState, bool refreshRoster)
    {
        var plan = MediaCorePollPolicy.Plan(spineConfigured, meetingState);

        Assert.True(plan.PollCoreSnapshot);
        Assert.Equal(refreshRoster, plan.RefreshZoomRoster);
    }

    [Fact]
    public async Task EngineOffInAMeeting_KeepsPollingTheCoreAtTheNormalCadence()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-poll-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "poll-core.cjs");
            var trace = Path.Combine(directory, "trace.txt");
            // A fake core: answers the handshake, reports an in-meeting roster, and answers each
            // media-core-sync with a wire state whose frame count advances.
            await File.WriteAllTextAsync(script, """
                const fs = require('node:fs'), readline = require('node:readline');
                const trace = process.env.COREVIDEO_POLL_TRACE;
                const profile = {name:'poll-fake',renderer:'software',maxProgramResolution:'1920x1080'};
                let frames = 0;
                console.log(JSON.stringify({id:'handshake',ok:true,type:'handshake',protocolVersion:{major:1,minor:0},profile}));
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const m = JSON.parse(line);
                  fs.appendFileSync(trace, m.type + '\n');
                  let response = {id:m.id, ok:true, type:m.type};
                  if (m.type === 'handshake') response = {...response, protocolVersion:{major:1,minor:0}, profile};
                  if (m.type === 'zoom-snapshot') response.snapshot = {meetingState:'in_meeting', participants:[{userId:'p1',displayName:'Guest'}]};
                  if (m.type === 'media-core-sync') {
                    frames += 15;
                    response.state = {health:{frameCount:frames}, profile, meetingState:'in_meeting', programFrameCount:frames};
                  }
                  console.log(JSON.stringify(response));
                });
                """);
            await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = Path.GetTempPath(),
                Environment = new Dictionary<string, string> { ["COREVIDEO_POLL_TRACE"] = trace },
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 3000, FrameDrainIntervalMs = 100000
            }));
            Assert.NotNull(await bridge.StartAsync());

            // Joined, Engine never turned on: no spine payload factory is configured.
            await bridge.GetZoomSnapshotAsync();
            Assert.Equal("in_meeting", bridge.LastSnapshot?.MeetingState);
            var before = await File.ReadAllLinesAsync(trace);
            var syncsBefore = before.Count(line => line == "media-core-sync");

            await Task.Delay(TimeSpan.FromMilliseconds(1750));

            var after = await File.ReadAllLinesAsync(trace);
            var polls = after.Count(line => line == "media-core-sync") - syncsBefore;
            // 250 ms cadence over 1.75 s is ~7 polls; 4 leaves room for a busy test machine.
            Assert.True(polls >= 4, $"expected the core to be polled at its normal cadence, saw {polls} media-core-sync request(s)");
            // The roster is still refreshed while capture is off.
            Assert.True(after.Count(line => line == "zoom-snapshot") >= 3);

            var snapshot = bridge.LastSnapshot!;
            Assert.Equal("in_meeting", snapshot.MeetingState);
            Assert.NotNull(snapshot.RawReceivedUtc);
            Assert.True(DateTimeOffset.UtcNow - snapshot.RawReceivedUtc!.Value < TimeSpan.FromSeconds(1),
                $"the core snapshot is stale: received {snapshot.RawReceivedUtc:O}");
            Assert.True(snapshot.ProgramFrameCount >= 60);
        }
        finally { Directory.Delete(directory, recursive: true); }
    }
}
