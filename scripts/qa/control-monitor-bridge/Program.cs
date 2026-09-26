using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

if (args.Length != 1 || !File.Exists(args[0]))
{
    Console.Error.WriteLine("Usage: dotnet run --project scripts/qa/control-monitor-bridge -- <native-core-path>");
    return 2;
}

var corePath = Path.GetFullPath(args[0]);
await using var bridge = new MediaCoreBridgeService(new MediaCoreSupervisor(
    new MediaCoreSupervisorOptions
    {
        Command = corePath, WorkingDirectory = Path.GetDirectoryName(corePath)
    }));

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

try
{
    await bridge.StartAsync();
    var initial = (await bridge.PollSnapshotAsync()).AudioMixSession.MonitorControl
        ?? throw new InvalidOperationException("Missing monitor control contract");
    var epoch = initial.AuthorityEpoch;
    var revision = initial.Revision;
    var draft = new MediaCoreAudioMonitorWire(false, "", "", 0.5);

    var first = await bridge.SetAudioMonitorControlAsync(draft, epoch, revision, "bridge-first");
    Require(first.Kind == AudioMonitorControlOutcomeKind.Applied &&
            first.Applied?.MonitorVolume == 0.5 && first.Control?.Revision == revision + 1,
        "First shell command did not apply");
    var stale = await bridge.SetAudioMonitorControlAsync(draft with { Volume = 0.25 },
        epoch, revision, "bridge-stale");
    Require(stale.Kind == AudioMonitorControlOutcomeKind.Conflict &&
            stale.Applied?.MonitorVolume == 0.5,
        "Stale shell command overwrote applied monitor state");
    var duplicate = await bridge.SetAudioMonitorControlAsync(draft,
        epoch, revision, "bridge-first");
    Require(duplicate.Kind == AudioMonitorControlOutcomeKind.Applied &&
            duplicate.Control?.Revision == revision + 1,
        "Duplicate operation changed the revision");
    var rebased = await bridge.SetAudioMonitorControlAsync(draft with { Volume = 0.25 },
        epoch, revision + 1, "bridge-rebased");
    Require(rebased.Kind == AudioMonitorControlOutcomeKind.Applied &&
            rebased.Control?.Revision == revision + 2,
        "Rebased shell command did not apply");

    bridge.Stop();
    await bridge.StartAsync();
    var afterRestart = (await bridge.PollSnapshotAsync()).AudioMixSession.MonitorControl
        ?? throw new InvalidOperationException("Restarted core lost monitor control contract");
    Require(afterRestart.AuthorityEpoch != epoch, "Core restart reused authority epoch");
    var oldEpoch = await bridge.SetAudioMonitorControlAsync(draft,
        epoch, revision, "bridge-old-epoch");
    Require(oldEpoch.Kind == AudioMonitorControlOutcomeKind.Conflict,
        "Retired core epoch command was accepted");
    var newEpoch = await bridge.SetAudioMonitorControlAsync(draft,
        afterRestart.AuthorityEpoch, afterRestart.Revision, "bridge-new-epoch");
    Require(newEpoch.Kind == AudioMonitorControlOutcomeKind.Applied,
        "New core epoch command did not apply");

    Console.WriteLine(JsonSerializer.Serialize(new
    {
        status = "passed", firstRevision = revision + 1, finalRevision = revision + 2,
        staleStatus = stale.Kind.ToString(), duplicateStatus = duplicate.Kind.ToString(),
        restartRejectedOldEpoch = true, newEpochApplied = true
    }));
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine($"Monitor bridge probe failed: {error.Message}");
    return 1;
}
