using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>NDI output validates on its own (spec 2026-11-01 behaviour 4): a failure refuses or
/// turns off NDI only, and stream validation never considers NDI.</summary>
public sealed class NdiOutputValidationTests
{
    [Fact]
    public void ValidNameIsAccepted()
    {
        Assert.Null(StudioStreamOutputValidation.ValidateNdiOutput("Studio A", null, false, true, true));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void BlankProgramNameIsRefusedWithTheNameReason(string? name)
    {
        Assert.Equal(
            "Configure an NDI program name before turning on NDI output.",
            StudioStreamOutputValidation.ValidateNdiOutput(name, null, false, true, true));
    }

    [Fact]
    public void KeyNameEqualToTheProgramNameIsRefusedWithTheCollisionReason()
    {
        Assert.Equal(
            "NDI graphics key name must differ from the NDI program name.",
            StudioStreamOutputValidation.ValidateNdiOutput("Studio A", " studio a ", true, true, true));
        // The key is off: its stored name cannot collide with anything on the network.
        Assert.Null(StudioStreamOutputValidation.ValidateNdiOutput("Studio A", "Studio A", false, true, true));
    }

    [Fact]
    public void CoreWithoutNdiOutputIsRefusedWithTheMissingCapabilityReason()
    {
        Assert.Equal(
            "NDI output is not available in this core build (missing ndi-output).",
            StudioStreamOutputValidation.ValidateNdiOutput("Studio A", null, false, false, null));
    }

    [Fact]
    public void CoreNotUpYetDefersToTheCoresOwnSenderStatus()
    {
        Assert.Null(StudioStreamOutputValidation.ValidateNdiOutput("Studio A", null, false, null, null));
    }

    // A key the core cannot carry is dropped from the destination list on its own; it never
    // refuses or stops the Program NDI source.
    [Fact]
    public void KeyTheCoreCannotCarryNeverFailsNdi()
    {
        Assert.Null(StudioStreamOutputValidation.ValidateNdiOutput("Program", null, true, true, false));
        Assert.Null(StudioStreamOutputValidation.ValidateNdiOutput("Program", "Program", true, true, false));
    }

    // Review focus 4: an invalid NDI name while NDI and a stream both run turns NDI off only.
    [Fact]
    public void NdiFailureTurnsOffNdiOnlyAndNeverStopsTheStream()
    {
        var outcome = StudioStreamOutputValidation.ResolveOutputValidation(
            streamingRequested: true, validateStream: () => null,
            ndiOutputEnabled: true, validateNdi: () => "Configure an NDI program name before turning on NDI output.");
        Assert.Null(outcome.StreamStopReason);
        Assert.Equal("Configure an NDI program name before turning on NDI output.", outcome.NdiOffReason);
    }

    [Fact]
    public void StreamFailureStopsTheStreamOnlyAndNeverTurnsOffNdi()
    {
        var outcome = StudioStreamOutputValidation.ResolveOutputValidation(
            streamingRequested: true, validateStream: () => "Select at least one stream destination.",
            ndiOutputEnabled: true, validateNdi: () => null);
        Assert.Equal("Select at least one stream destination.", outcome.StreamStopReason);
        Assert.Null(outcome.NdiOffReason);
    }

    [Fact]
    public void OutputsThatAreOffAreNotValidated()
    {
        var outcome = StudioStreamOutputValidation.ResolveOutputValidation(
            streamingRequested: false, validateStream: () => throw new InvalidOperationException("stream validated while off"),
            ndiOutputEnabled: false, validateNdi: () => throw new InvalidOperationException("NDI validated while off"));
        Assert.Null(outcome.StreamStopReason);
        Assert.Null(outcome.NdiOffReason);
    }

    // Stream validation no longer considers NDI: "Select at least one stream destination" means
    // RTMP, SRT or HLS, and NDI on its own is not a stream.
    [Fact]
    public void StreamValidationKnowsNothingAboutNdi()
    {
        Assert.Equal("Select at least one stream destination.", StudioStreamOutputValidation.ValidateSelectedDestinations(
            rtmpEnabled: false, srtEnabled: false, rtmpProtocol: "rtmps", rtmpServerUrl: "", rtmpStreamKey: "",
            srtMode: "caller", srtHost: "", srtPort: "", srtLatencyMs: "", srtStreamId: "", srtKeyLength: "0", srtPassphrase: ""));
    }
}
