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
            var before = await ReadTraceLinesAsync(trace);
            var syncsBefore = before.Count(line => line == "media-core-sync");

            // The property is "the poll keeps running with Engine off" — the defect polled the core
            // ZERO times here. Wait for polls against a generous deadline instead of counting them in
            // a fixed window: a CI runner saw 2 polls in 1.75 s where this machine sees ~7.
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
            string[] after;
            int polls;
            do
            {
                await Task.Delay(TimeSpan.FromMilliseconds(250));
                after = await ReadTraceLinesAsync(trace);
                polls = after.Count(line => line == "media-core-sync") - syncsBefore;
            }
            // The fake traces a request when it RECEIVES it, before the bridge applies the reply,
            // so also wait for the applied snapshot to carry the advanced frame count.
            while ((polls < 4 || after.Count(line => line == "zoom-snapshot") < 3 ||
                    (bridge.LastSnapshot?.ProgramFrameCount ?? 0) < 60) && DateTime.UtcNow < deadline);

            Assert.True(polls >= 4, $"expected the core to keep being polled with Engine off, saw {polls} media-core-sync request(s) in 15 s");
            // The roster is still refreshed while capture is off.
            Assert.True(after.Count(line => line == "zoom-snapshot") >= 3);

            var snapshot = bridge.LastSnapshot!;
            Assert.Equal("in_meeting", snapshot.MeetingState);
            Assert.NotNull(snapshot.RawReceivedUtc);
            // 2 s = the /snapshot envelope's own "stale" threshold.
            Assert.True(DateTimeOffset.UtcNow - snapshot.RawReceivedUtc!.Value < TimeSpan.FromSeconds(2),
                $"the core snapshot is stale: received {snapshot.RawReceivedUtc:O}");
            Assert.True(snapshot.ProgramFrameCount >= 60);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); }
            catch (IOException) { /* node may still hold the trace until process teardown */ }
        }
    }

    // File.ReadAllLinesAsync uses FileShare.Read; the fake core appends with
    // fs.appendFileSync. On Windows that races as IOException "used by another process".
    private static async Task<string[]> ReadTraceLinesAsync(string path)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                await using var stream = new FileStream(
                    path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream);
                var text = await reader.ReadToEndAsync();
                return text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
            }
            catch (IOException) when (attempt < 8)
            {
                await Task.Delay(50);
            }
        }
    }
}
