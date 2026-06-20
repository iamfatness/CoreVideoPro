using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomMeetingUrlParserTests
{
    [Fact]
    public void Parse_ExtractsPasscodeAndMeetingNumberFromZoomUrl()
    {
        var parsed = ZoomMeetingUrlParser.Parse(
            "https://us06web.zoom.us/j/5228151336?pwd=SlFIbDRuQU9FZEZReGpFRUY4NDM3dz09");

        Assert.Equal("5228151336", parsed.MeetingNumber);
        Assert.Equal("SlFIbDRuQU9FZEZReGpFRUY4NDM3dz09", parsed.Passcode);
    }

    [Fact]
    public void Parse_ExtractsMeetingNumberFromBareId()
    {
        var parsed = ZoomMeetingUrlParser.Parse("965 5347 4365");

        Assert.Equal("96553474365", parsed.MeetingNumber);
        Assert.Equal("965 5347 4365", parsed.MeetingUrl);
        Assert.Null(parsed.Passcode);
    }
}
