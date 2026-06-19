using CoreVideoPro.MediaCore.Services;
using System.Text.Json;
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

    [Fact]
    public async Task LoadAsync_SanitizesLegacyStoreAndDropsInvalidMeetings()
    {
        var path = CreateTempPath();
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            await File.WriteAllTextAsync(
                path,
                """
                [
                  {
                    "meetingNumber": "123456789",
                    "displayName": "  Executive Producer  ",
                    "webinar": true,
                    "joinedAt": "2026-06-19T14:00:00Z",
                    "meetingUrl": "https://zoom.us/j/123456789?pwd=legacy-secret",
                    "passcode": "legacy-secret"
                  },
                  {
                    "meetingNumber": "https://zoom.us/j/987654321?pwd=bad",
                    "displayName": "Bad Legacy Record",
                    "webinar": false,
                    "joinedAt": "2026-06-19T14:05:00Z"
                  },
                  {
                    "meetingNumber": "123",
                    "displayName": "Too Short",
                    "webinar": false,
                    "joinedAt": "2026-06-19T14:10:00Z"
                  }
                ]
                """);

            var store = new FileRecentZoomMeetingStore(path);
            var meetings = await store.LoadAsync();

            var meeting = Assert.Single(meetings);
            Assert.Equal("123456789", meeting.MeetingNumber);
            Assert.Equal("Executive Producer", meeting.DisplayName);
            Assert.True(meeting.Webinar);

            var serialized = JsonSerializer.Serialize(meeting);
            Assert.DoesNotContain("legacy-secret", serialized);
            Assert.DoesNotContain("passcode", serialized, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("meetingUrl", serialized, StringComparison.OrdinalIgnoreCase);
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
