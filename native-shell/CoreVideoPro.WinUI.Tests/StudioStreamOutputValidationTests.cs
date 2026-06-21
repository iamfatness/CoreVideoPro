using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioStreamOutputValidationTests
{
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
}
