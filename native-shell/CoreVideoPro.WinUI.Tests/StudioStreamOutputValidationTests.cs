using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioStreamOutputValidationTests
{
    [Fact]
    public void Destinations_RejectsEmptySelectionBeforeStreaming()
    {
        var error = StudioStreamOutputValidation.ValidateSelectedDestinations(
            rtmpEnabled: false,
            ndiEnabled: false,
            srtEnabled: false,
            rtmpProtocol: "rtmps",
            rtmpServerUrl: "",
            rtmpStreamKey: "",
            ndiProgramName: "",
            srtMode: "caller",
            srtHost: "",
            srtPort: "",
            srtLatencyMs: "",
            srtStreamId: "",
            srtKeyLength: "0",
            srtPassphrase: "");

        Assert.Equal("Select at least one stream destination.", error);
    }

    [Fact]
    public void Destinations_RejectsInvalidEnabledDestinationBeforeLiveResync()
    {
        var error = StudioStreamOutputValidation.ValidateSelectedDestinations(
            rtmpEnabled: true,
            ndiEnabled: false,
            srtEnabled: false,
            rtmpProtocol: "rtmps",
            rtmpServerUrl: "live.example.com",
            rtmpStreamKey: "stream-key",
            ndiProgramName: "",
            srtMode: "caller",
            srtHost: "",
            srtPort: "",
            srtLatencyMs: "",
            srtStreamId: "",
            srtKeyLength: "0",
            srtPassphrase: "");

        Assert.Equal("RTMP server URL must include an application path.", error);
    }

    [Fact]
    public void Destinations_AcceptsConfiguredRtmpNdiAndSrt()
    {
        var error = StudioStreamOutputValidation.ValidateSelectedDestinations(
            rtmpEnabled: true,
            ndiEnabled: true,
            srtEnabled: true,
            rtmpProtocol: "rtmps",
            rtmpServerUrl: "live.example.com/app",
            rtmpStreamKey: "stream-key",
            ndiProgramName: "CoreVideo Pro Program",
            srtMode: "caller",
            srtHost: "receiver.example.com",
            srtPort: "9000",
            srtLatencyMs: "120",
            srtStreamId: "publish/live/main",
            srtKeyLength: "16",
            srtPassphrase: "secret-passphrase");

        Assert.Null(error);
    }

    [Fact]
    public void Rtmp_AllowsSchemeLessServerUrlAndBuildsSelectedProtocolUrl()
    {
        var error = StudioStreamOutputValidation.ValidateRtmp(
            "rtmps",
            "live.example.com/app",
            "stream-key");

        Assert.Null(error);
        Assert.Equal(
            "rtmps://live.example.com/app",
            StudioStreamOutputValidation.BuildRtmpUrl("rtmps", "live.example.com/app"));
    }

    [Fact]
    public void Rtmp_RejectsUrlSchemeThatDoesNotMatchSelectedProtocol()
    {
        var error = StudioStreamOutputValidation.ValidateRtmp(
            "rtmp",
            "rtmps://live.example.com/app",
            "stream-key");

        Assert.Equal("RTMP protocol selection must match the server URL scheme.", error);
    }

    [Theory]
    [InlineData("not a valid host/app")]
    [InlineData("rtmps:///live/app")]
    [InlineData("http://live.example.com/app")]
    public void Rtmp_RejectsMalformedOrUnsupportedServerUrl(string serverUrl)
    {
        var error = StudioStreamOutputValidation.ValidateRtmp(
            "rtmps",
            serverUrl,
            "stream-key");

        Assert.NotNull(error);
    }

    [Theory]
    [InlineData("live.example.com")]
    [InlineData("rtmps://live.example.com")]
    public void Rtmp_RejectsServerUrlWithoutApplicationPath(string serverUrl)
    {
        var error = StudioStreamOutputValidation.ValidateRtmp(
            "rtmps",
            serverUrl,
            "stream-key");

        Assert.Equal("RTMP server URL must include an application path.", error);
    }

    [Fact]
    public void Rtmp_RejectsStreamKeyWithWhitespace()
    {
        var error = StudioStreamOutputValidation.ValidateRtmp(
            "rtmps",
            "live.example.com/app",
            "stream key");

        Assert.Equal("RTMP stream key cannot contain whitespace.", error);
    }

    [Fact]
    public void Rtmp_DoesNotSerializeSettingsUntilFullyValid()
    {
        Assert.False(StudioStreamOutputValidation.CanSerializeRtmpSettings(
            "rtmps",
            "live.example.com",
            "stream-key"));

        Assert.True(StudioStreamOutputValidation.CanSerializeRtmpSettings(
            "rtmps",
            "live.example.com/app",
            "stream-key"));
    }

    [Theory]
    [InlineData("caller")]
    [InlineData("listener")]
    [InlineData("rendezvous")]
    public void Srt_AcceptsSupportedModesWithoutEncryption(string mode)
    {
        var error = StudioStreamOutputValidation.ValidateSrt(
            mode,
            "receiver.example.com",
            "9000",
            "120",
            "",
            "0",
            "");

        Assert.Null(error);
    }

    [Fact]
    public void Srt_RequiresPassphraseWhenEncryptionEnabled()
    {
        var error = StudioStreamOutputValidation.ValidateSrt(
            "caller",
            "receiver.example.com",
            "9000",
            "120",
            "",
            "16",
            "");

        Assert.Equal("Configure an SRT passphrase when encryption is enabled.", error);
    }

    [Fact]
    public void Srt_RejectsPassphraseWhenEncryptionDisabled()
    {
        var error = StudioStreamOutputValidation.ValidateSrt(
            "caller",
            "receiver.example.com",
            "9000",
            "120",
            "",
            "0",
            "unused-passphrase");

        Assert.Equal("Choose an SRT key length before entering a passphrase.", error);
    }

    [Fact]
    public void Srt_DoesNotSerializeSettingsUntilFullyValid()
    {
        Assert.False(StudioStreamOutputValidation.CanSerializeSrtSettings(
            "caller",
            "receiver.example.com",
            "",
            "120",
            "",
            "0",
            ""));

        Assert.True(StudioStreamOutputValidation.CanSerializeSrtSettings(
            "caller",
            "receiver.example.com",
            "9000",
            "120",
            "",
            "0",
            ""));
    }

    [Fact]
    public void Ndi_DoesNotSerializeSettingsWithoutProgramName()
    {
        Assert.False(StudioStreamOutputValidation.CanSerializeNdiSettings(""));
        Assert.True(StudioStreamOutputValidation.CanSerializeNdiSettings("CoreVideo Pro Program"));
    }
}
