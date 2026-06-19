using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class RecentZoomMeetingStoreTests
{
    [Fact]
    public async Task RememberAsync_StoresSanitizedRejoinFields()
    {
        var path = CreateTempPath();
        try
        {
            var store = new FileRecentZoomMeetingStore(path);

            var meetings = await store.RememberAsync(
                "https://zoom.us/j/987654321?pwd=secret-passcode",
                " Guest Producer ",
                webinar: true,
                joinedAt: DateTimeOffset.Parse("2026-06-19T12:00:00Z"));

            var meeting = Assert.Single(meetings);
            Assert.Equal("987654321", meeting.MeetingNumber);
            Assert.Equal("Guest Producer", meeting.DisplayName);
            Assert.True(meeting.Webinar);

            var json = await File.ReadAllTextAsync(path);
            Assert.Contains("987654321", json);
            Assert.DoesNotContain("secret-passcode", json);
            Assert.DoesNotContain("pwd", json);
        }
        finally
        {
            DeleteTempPath(path);
        }
    }

    [Fact]
    public async Task RememberAsync_DeduplicatesAndKeepsMostRecentTen()
    {
        var path = CreateTempPath();
        try
        {
            var store = new FileRecentZoomMeetingStore(path);

            for (var index = 0; index < FileRecentZoomMeetingStore.MaxRecentMeetings + 2; index++)
            {
                await store.RememberAsync(
                    $"https://zoom.us/j/{100000000 + index}",
                    $"Producer {index}",
                    webinar: false,
                    joinedAt: DateTimeOffset.Parse("2026-06-19T12:00:00Z").AddMinutes(index));
            }

            var meetings = await store.RememberAsync(
                "https://zoom.us/j/100000005?pwd=ignored",
                "Updated Producer",
                webinar: true,
                joinedAt: DateTimeOffset.Parse("2026-06-19T13:00:00Z"));

            Assert.Equal(FileRecentZoomMeetingStore.MaxRecentMeetings, meetings.Count);
            Assert.Equal("100000005", meetings[0].MeetingNumber);
            Assert.Equal("Updated Producer", meetings[0].DisplayName);
            Assert.True(meetings[0].Webinar);
            Assert.Single(meetings, meeting => meeting.MeetingNumber == "100000005");
        }
        finally
        {
            DeleteTempPath(path);
        }
    }

    private static string CreateTempPath()
    {
        var directory = Path.Combine(Path.GetTempPath(), "CoreVideoPro.Tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        return Path.Combine(directory, "recent-meetings.json");
    }

    private static void DeleteTempPath(string path)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory) && Directory.Exists(directory))
        {
            Directory.Delete(directory, recursive: true);
        }
    }
}
