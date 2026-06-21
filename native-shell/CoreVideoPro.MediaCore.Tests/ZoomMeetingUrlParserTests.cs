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

    [Fact]
    public void Parse_ReportsEmptyMeetingAsNotJoinable()
    {
        var parsed = ZoomMeetingUrlParser.Parse(" ");

        Assert.False(parsed.CanJoin);
        Assert.Equal("Enter a Zoom meeting URL or meeting ID before joining.", parsed.ValidationError);
        Assert.Null(parsed.ZoomAppUri);
    }

    [Fact]
    public void Parse_RejectsTextWithoutMeetingNumber()
    {
        var parsed = ZoomMeetingUrlParser.Parse("not a zoom meeting");

        Assert.False(parsed.CanJoin);
        Assert.Equal("Enter a valid Zoom meeting URL or a 9-12 digit meeting ID.", parsed.ValidationError);
    }

    [Fact]
    public void Parse_BuildsZoomAppUriWithoutLeakingFullUrl()
    {
        var parsed = ZoomMeetingUrlParser.Parse(
            "https://us06web.zoom.us/j/5228151336?pwd=abc 123");

        Assert.True(parsed.CanJoin);
        Assert.Equal("zoommtg://zoom.us/join?confno=5228151336&pwd=abc%20123", parsed.ZoomAppUri);
    }
}
