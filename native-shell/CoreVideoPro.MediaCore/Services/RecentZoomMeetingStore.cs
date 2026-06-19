using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class RecentZoomMeeting
{
    public required string MeetingNumber { get; init; }
    public required string DisplayName { get; init; }
    public bool Webinar { get; init; }
    public DateTimeOffset JoinedAt { get; init; }

    public string Summary => $"{MeetingNumber} - {DisplayName}";
}

public interface IRecentZoomMeetingStore
{
    Task<IReadOnlyList<RecentZoomMeeting>> LoadAsync(CancellationToken cancellationToken = default);
    Task<IReadOnlyList<RecentZoomMeeting>> RememberAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        DateTimeOffset? joinedAt = null,
        CancellationToken cancellationToken = default);
}

public sealed class FileRecentZoomMeetingStore : IRecentZoomMeetingStore
{
    public const int MaxRecentMeetings = 10;

    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true
    };

    private readonly string _filePath;

    public FileRecentZoomMeetingStore(string filePath)
    {
        _filePath = filePath;
    }

    public static string DefaultStorePath()
    {
        var root = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro");
        return Path.Combine(root, "recent-meetings.json");
    }

    public async Task<IReadOnlyList<RecentZoomMeeting>> LoadAsync(CancellationToken cancellationToken = default)
    {
        if (!File.Exists(_filePath))
        {
            return [];
        }

        try
        {
            await using var stream = File.OpenRead(_filePath);
            var parsed = await JsonSerializer.DeserializeAsync<List<RecentZoomMeeting>>(stream, JsonOptions, cancellationToken)
                .ConfigureAwait(false);
            return Sanitize(parsed ?? []);
        }
        catch
        {
            return [];
        }
    }

    public async Task<IReadOnlyList<RecentZoomMeeting>> RememberAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        DateTimeOffset? joinedAt = null,
        CancellationToken cancellationToken = default)
    {
        var meetingNumber = ExtractMeetingNumber(meetingUrl);
        if (string.IsNullOrWhiteSpace(meetingNumber))
        {
            return await LoadAsync(cancellationToken).ConfigureAwait(false);
        }

        var current = await LoadAsync(cancellationToken).ConfigureAwait(false);
        var remembered = new RecentZoomMeeting
        {
            MeetingNumber = meetingNumber,
            DisplayName = string.IsNullOrWhiteSpace(displayName) ? "CoreVideo Producer" : displayName.Trim(),
            Webinar = webinar,
            JoinedAt = joinedAt ?? DateTimeOffset.UtcNow
        };

        var next = new[] { remembered }
            .Concat(current.Where(meeting => !meeting.MeetingNumber.Equals(meetingNumber, StringComparison.Ordinal)))
            .Take(MaxRecentMeetings)
            .ToList();

        Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
        await using var stream = File.Create(_filePath);
        await JsonSerializer.SerializeAsync(stream, next, JsonOptions, cancellationToken).ConfigureAwait(false);
        return next;
    }

    private static string? ExtractMeetingNumber(string meetingUrl)
    {
        var parsed = ZoomMeetingUrlParser.Parse(meetingUrl);
        if (!string.IsNullOrWhiteSpace(parsed.MeetingNumber))
        {
            return parsed.MeetingNumber;
        }

        var digitsOnly = new string(meetingUrl.Where(char.IsDigit).ToArray());
        return digitsOnly.Length >= 6 ? digitsOnly : null;
    }

    private static IReadOnlyList<RecentZoomMeeting> Sanitize(IEnumerable<RecentZoomMeeting> meetings) =>
        meetings
            .Where(meeting => !string.IsNullOrWhiteSpace(meeting.MeetingNumber) &&
                              meeting.MeetingNumber.All(char.IsDigit) &&
                              meeting.MeetingNumber.Length >= 6)
            .Select(meeting => new RecentZoomMeeting
            {
                MeetingNumber = meeting.MeetingNumber,
                DisplayName = string.IsNullOrWhiteSpace(meeting.DisplayName) ? "CoreVideo Producer" : meeting.DisplayName.Trim(),
                Webinar = meeting.Webinar,
                JoinedAt = meeting.JoinedAt
            })
            .Take(MaxRecentMeetings)
            .ToList();
}
