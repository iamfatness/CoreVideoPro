using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// S3 telemetry payload builder: correct counts/kinds from the snapshot, the
/// machine-class reuse, and — the KEY invariant — that NO secret or endpoint from
/// a secret-laden snapshot ever appears in the serialized payload.
/// </summary>
public sealed class TelemetryPayloadBuilderTests
{
    // Seeded secrets that MUST NEVER appear in a telemetry payload.
    private const string StreamKey = "live_SUPERSECRETSTREAMKEY_9f3a2b";
    private const string RtmpUrl = "rtmp://a.rtmp.youtube.com/live2/" + StreamKey;
    private const string SrtPassphrase = "srt-passphrase-do-not-leak-42";
    private const string RecordingFolder = @"C:\Users\producer\Videos\Confidential Client Show";
    private const string ProgramPath = RecordingFolder + @"\Program.mp4";
    private const string GuestName = "Alice Q. Attendee";
    private const string MeetingUrl = "https://zoom.us/j/98765432100?pwd=secretmeetingpassword";

    private static NativeMediaCoreStateSnapshot SecretLadenSnapshot() => new()
    {
        SceneId = "scene-main",
        MeetingState = MeetingUrl, // deliberately stuff a secret-shaped string here too
        Recording = new NativeMediaCoreRecordingSession
        {
            SessionId = "rec-1",
            Active = true,
            Status = "recording",
            WriterStatus = "writing",
            TargetFolder = RecordingFolder,
            FilenamePrefix = "Show",
            Format = "mp4",
            Quality = "high",
            ProgramPath = ProgramPath,
            Streams =
            [
                new NativeMediaCoreRecordingStream
                {
                    Kind = "iso",
                    SourceId = "zoom:pid-secret-1",
                    ParticipantId = "pid-secret-1",
                    DisplayName = GuestName,
                    Path = RecordingFolder + @"\ISO-01-Alice.mp4",
                    Status = "writing",
                    HasAudio = true
                }
            ]
        },
        OutputSenderSession = new NativeMediaCoreOutputSenderSession
        {
            Status = "live",
            ActiveSenderCount = 1,
            Senders =
            [
                new NativeMediaCoreOutputSender
                {
                    SenderId = "rtmp-1",
                    Destination = RtmpUrl,
                    Status = "live",
                    Warning = SrtPassphrase
                }
            ]
        },
        VirtualCamera = new NativeMediaCoreVirtualCamera { Enabled = true, Status = "live" },
        IsoParticipantIds = ["zoom:pid-secret-1"],
        Frames =
        [
            new NativeMediaCoreFrame { SourceId = "zoom:pid-secret-1", Kind = "zoom", Health = "live" },
            new NativeMediaCoreFrame { SourceId = "capture:cam-a", Kind = "capture", Health = "live" },
            new NativeMediaCoreFrame { SourceId = "capture:cam-b", Kind = "capture", Health = "live" },
            new NativeMediaCoreFrame { SourceId = "capture:cam-b", Kind = "capture", Health = "live" } // dup id
        ],
        Participants =
        [
            new RawParticipantEvent { UserId = "pid-secret-1", DisplayName = GuestName },
            new RawParticipantEvent { UserId = "pid-secret-2", DisplayName = "Bob Secret" }
        ]
    };

    private static TelemetryMachineClass Machine() => new()
    {
        Label = "win-x64-cpu16-ram32gb",
        CpuCores = 16,
        RamBand = "32-64GB",
        GpuTier = "nvidia-vram16gb+"
    };

    [Fact]
    public void OutputConfigShape_ReadsCountsAndKindsFromSnapshot()
    {
        var shape = TelemetryPayloadBuilder.BuildOutputConfigShape(SecretLadenSnapshot());

        Assert.True(shape.RecordingEnabled);
        Assert.True(shape.StreamingEnabled);
        Assert.True(shape.VcamEnabled);
        Assert.Equal(1, shape.IsoSourceCount);
        Assert.Equal(2, shape.CaptureSourceCount); // cam-a + cam-b, dup collapsed
        Assert.Equal(2, shape.ZoomParticipantCount);
    }

    [Fact]
    public void OutputConfigShape_NullSnapshot_IsEmptyAndDisabled()
    {
        var shape = TelemetryPayloadBuilder.BuildOutputConfigShape(null);

        Assert.False(shape.RecordingEnabled);
        Assert.False(shape.StreamingEnabled);
        Assert.False(shape.VcamEnabled);
        Assert.Equal(0, shape.IsoSourceCount);
        Assert.Equal(0, shape.CaptureSourceCount);
        Assert.Equal(0, shape.ZoomParticipantCount);
    }

    [Fact]
    public void Build_PopulatesEveryDeclaredField()
    {
        var payload = TelemetryPayloadBuilder.Build(
            TelemetryEventNames.SessionEnd,
            "1.2.3",
            sessionLengthSeconds: 3600,
            SecretLadenSnapshot(),
            crashCountSinceLastSend: 2,
            Machine());

        Assert.Equal("session-end", payload.Name);
        Assert.Equal("1.2.3", payload.Version);
        Assert.Equal(3600, payload.SessionLengthSeconds);
        Assert.Equal(2, payload.CrashCountSinceLastSend);
        Assert.Equal("win-x64-cpu16-ram32gb", payload.MachineClass);
        Assert.Equal(16, payload.Machine.CpuCores);
        Assert.Equal("32-64GB", payload.Machine.RamBand);
        Assert.True(payload.OutputConfigShape.RecordingEnabled);
    }

    [Fact]
    public void Build_ClampsNegativeInputs()
    {
        var payload = TelemetryPayloadBuilder.Build(
            TelemetryEventNames.Heartbeat, "1.0.0", -5, null, -3, Machine());

        Assert.Equal(0, payload.SessionLengthSeconds);
        Assert.Equal(0, payload.CrashCountSinceLastSend);
    }

    /// <summary>
    /// THE key test: a snapshot stuffed with a stream key, an RTMP URL, an SRT
    /// passphrase, a recording folder path, guest names, and a meeting URL — none
    /// of them may appear anywhere in the serialized telemetry JSON.
    /// </summary>
    [Fact]
    public void Serialize_NeverLeaksAnySecretOrEndpoint()
    {
        var payload = TelemetryPayloadBuilder.Build(
            TelemetryEventNames.SessionEnd,
            "1.2.3",
            sessionLengthSeconds: 42,
            SecretLadenSnapshot(),
            crashCountSinceLastSend: 0,
            Machine());

        foreach (var json in new[]
                 {
                     TelemetryPayloadBuilder.SerializePreview(payload),
                     TelemetryPayloadBuilder.SerializeWire(payload)
                 })
        {
            Assert.DoesNotContain(StreamKey, json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("rtmp://", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("a.rtmp.youtube.com", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain(SrtPassphrase, json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain(RecordingFolder, json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("Program.mp4", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain(GuestName, json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("Bob Secret", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("zoom.us", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("secretmeetingpassword", json, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("pid-secret", json, StringComparison.OrdinalIgnoreCase);
            // Sanity: the payload IS present (the shape, not the secrets).
            Assert.Contains("outputConfigShape", json, StringComparison.Ordinal);
            Assert.Contains("win-x64-cpu16-ram32gb", json, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Serialize_StaysWellUnderTheEventCap()
    {
        var payload = TelemetryPayloadBuilder.Build(
            TelemetryEventNames.SessionEnd, "1.2.3", 42, SecretLadenSnapshot(), 0, Machine());

        var wire = TelemetryPayloadBuilder.SerializeWire(payload);
        Assert.True(wire.Length < 64 * 1024, $"payload must be << 64KB, was {wire.Length}");
    }

    [Fact]
    public void MachineClassProbe_LabelMatchesS1Format()
    {
        var label = MachineClassProbe.Describe();
        Assert.Matches(@"^win-x64(-cpu\d+-ram\d+gb)?$", label);

        var probe = MachineClassProbe.Probe("intel-vram2-4gb");
        Assert.Equal(label, probe.Label); // same string S1 sends — one source of truth
        Assert.Equal(Environment.ProcessorCount, probe.CpuCores);
        Assert.Equal("intel-vram2-4gb", probe.GpuTier);
        Assert.False(string.IsNullOrWhiteSpace(probe.RamBand));
    }

    [Theory]
    [InlineData(0, "unknown")]
    [InlineData(4, "<8GB")]
    [InlineData(8, "8-16GB")]
    [InlineData(16, "16-32GB")]
    [InlineData(32, "32-64GB")]
    [InlineData(64, "64-128GB")]
    [InlineData(256, "128GB+")]
    public void MachineClassProbe_RamBands(int ramGb, string expected) =>
        Assert.Equal(expected, MachineClassProbe.RamBand(ramGb));
}
