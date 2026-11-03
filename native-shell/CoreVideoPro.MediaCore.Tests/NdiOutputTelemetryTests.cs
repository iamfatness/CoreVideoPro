using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NdiOutputTelemetryTests
{
    private static NativeMediaCoreOutputSender Sender(string destination, string status, string? warning = null) => new()
    {
        SenderId = destination,
        Destination = destination,
        Status = status,
        Warning = warning
    };

    [Fact]
    public void OffReadsOff() =>
        Assert.Equal("Off", StudioOutputTelemetry.NdiStatus(false, [], "Studio A", null));

    [Fact]
    public void OnWithNoSenderYetReadsStarting() =>
        Assert.Equal("Starting", StudioOutputTelemetry.NdiStatus(true, [], "Studio A", null));

    [Fact]
    public void LiveSenderNamesTheProgram() =>
        Assert.Equal("Live · Studio A",
            StudioOutputTelemetry.NdiStatus(true, [Sender("ndi", "live")], "Studio A", null));

    [Fact]
    public void LiveGraphicsKeyAppendsKey() =>
        Assert.Equal("Live · Studio A + key",
            StudioOutputTelemetry.NdiStatus(true, [Sender("ndi", "live"), Sender("ndi-graphics", "live")], "Studio A", null));

    [Fact]
    public void WarningCarriesTheSenderReason() =>
        Assert.Equal("Warning · runtime missing",
            StudioOutputTelemetry.NdiStatus(true, [Sender("ndi", "warning", "runtime missing")], "Studio A", null));

    [Fact]
    public void RefusalWhileOffReadsUnavailable() =>
        Assert.Equal("Unavailable · NDI program name is blank",
            StudioOutputTelemetry.NdiStatus(false, [], "Studio A", "NDI program name is blank"));

    [Fact]
    public void StreamSendersAreIgnored() =>
        Assert.Equal("Starting",
            StudioOutputTelemetry.NdiStatus(true, [Sender("rtmp", "live")], "Studio A", null));
}
