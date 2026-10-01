using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// Found by scripts/qa/core-restart-drill.py, 2026-10-01: in 2 of 6 live runs the core
/// restarted after a crash and never rejoined Zoom. Between the supervisor marking the dead
/// core and respawning it, it raises HealthChanged with its lock released. A shell caller that
/// "ensures the core is running" in that window (StartAsync) saw no live process and spawned
/// one itself; the crash handler then found a child it did not spawn and returned without
/// running the recovery. The core was up and the meeting was gone.
/// </summary>
public sealed class CoreRecoveryRejoinTests
{
    [Fact]
    public async Task AStartDuringCrashRecoveryStillRejoinsZoom()
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-recovery-rejoin-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var script = Path.Combine(directory, "core.cjs");
            // Every process appends one line per zoom-join it receives, so the test can count
            // joins across process generations.
            await File.WriteAllTextAsync(script, """
                const readline = require('node:readline');
                const fs = require('node:fs');
                const path = require('node:path');
                const joins = path.join(__dirname, 'joins.log');
                const profile = {name:'recovery-rejoin-fake',renderer:'software',maxProgramResolution:'1920x1080'};
                const roster = {meetingState:'in_meeting', rosterEpoch:'1:1:' + process.pid, rosterRevision:1,
                  participants:[{userId:'1',displayName:'Guest',sourceGeneration:1,muted:false,videoOn:true}]};
                readline.createInterface({input:process.stdin}).on('line', line => {
                  const m = JSON.parse(line);
                  let response = {id:m.id,ok:true,type:m.type};
                  if (m.type === 'handshake') response = {...response,protocolVersion:{major:1,minor:0},profile};
                  if (m.type === 'media-core-sync') {
                    if (m.commands?.some(c => c.type === 'test-die')) process.exit(9);
                    response.state = {health:{frameCount:1},profile,meetingState:'in_meeting',programFrameCount:1};
                  }
                  if (m.type === 'zoom-join') { fs.appendFileSync(joins, process.pid + '\n'); response.snapshot = roster; }
                  if (m.type === 'zoom-snapshot') response.snapshot = roster;
                  console.log(JSON.stringify(response));
                });
                """);
            var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "node", Args = [script], WorkingDirectory = directory,
                HandshakeRequestTimeoutMs = 5000, RequestTimeoutMs = 3000,
                FrameDrainIntervalMs = 100000, ZoomRecoveryRetryDelayMs = 50
            });
            await using var bridge = new MediaCoreBridgeService(supervisor);
            Assert.NotNull(await bridge.StartAsync());
            await bridge.JoinZoomAsync("https://example.zoom.us/j/123456789", "Producer", webinar: false);
            Assert.Single(JoinLines(directory));

            // The race, made deterministic: the moment the supervisor reports the crash (lock
            // released, before its own respawn) a caller ensures the core is running.
            Task? competingStart = null;
            supervisor.HealthChanged += health =>
            {
                if (health.Recovering && competingStart is null)
                    competingStart = supervisor.StartAsync();
            };

            try { await bridge.SyncAsync([new NativeMediaCoreCommand { Type = "test-die" }]); }
            catch (Exception) { /* the core died mid-request, as a crash does */ }

            await UntilAsync(() => competingStart is not null);
            try { await competingStart!; } catch (Exception) { /* its own outcome is not the point */ }

            // The recovery must still run: a second zoom-join, sent to a different process.
            await UntilAsync(() => JoinLines(directory).Length >= 2);
            var joins = JoinLines(directory);
            Assert.Equal(2, joins.Length);
            Assert.NotEqual(joins[0], joins[1]);
            await UntilAsync(() => !supervisor.Health.Recovering);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); }
            catch (IOException) { }
        }
    }

    private static string[] JoinLines(string directory)
    {
        var path = Path.Combine(directory, "joins.log");
        try { return File.Exists(path) ? File.ReadAllLines(path) : []; }
        catch (IOException) { return []; }
    }

    private static async Task UntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
        while (!condition() && DateTime.UtcNow < deadline) await Task.Delay(40);
        Assert.True(condition());
    }
}
