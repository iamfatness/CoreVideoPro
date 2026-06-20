namespace CoreVideoPro.MediaCore.Services;

public sealed class ZoomMeetingJoinDetails
{
    public string MeetingUrl { get; init; } = string.Empty;

    public string? Passcode { get; init; }

    public string? MeetingNumber { get; init; }
}

public static class ZoomMeetingUrlParser
{
    public static ZoomMeetingJoinDetails Parse(string meetingUrl)
    {
        var trimmed = meetingUrl.Trim();
        if (string.IsNullOrWhiteSpace(trimmed))
        {
            return new ZoomMeetingJoinDetails();
        }

        string? passcode = null;
        string? meetingNumber = null;

        if (Uri.TryCreate(trimmed, UriKind.Absolute, out var uri))
        {
            passcode = ReadQueryValue(uri.Query, "pwd");
            meetingNumber = ExtractMeetingNumber(uri.AbsolutePath);
        }
        else
        {
            meetingNumber = ExtractMeetingNumber(trimmed);
        }

        return new ZoomMeetingJoinDetails
        {
            MeetingUrl = trimmed,
            Passcode = passcode,
            MeetingNumber = meetingNumber
        };
    }

    private static string? ReadQueryValue(string query, string key)
    {
        if (string.IsNullOrWhiteSpace(query))
        {
            return null;
        }

        var trimmed = query.StartsWith('?') ? query[1..] : query;
        foreach (var segment in trimmed.Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var parts = segment.Split('=', 2);
            if (parts.Length == 2 &&
                parts[0].Equals(key, StringComparison.OrdinalIgnoreCase))
            {
                return Uri.UnescapeDataString(parts[1].Replace('+', ' '));
            }
        }

        return null;
    }

    private static string? ExtractMeetingNumber(string absolutePath)
    {
        if (string.IsNullOrWhiteSpace(absolutePath))
        {
            return null;
        }

        var segments = absolutePath.Split('/', StringSplitOptions.RemoveEmptyEntries);
        for (var index = segments.Length - 1; index >= 0; index--)
        {
            var digits = new string(segments[index].Where(char.IsDigit).ToArray());
            if (digits.Length >= 9)
            {
                return digits;
            }
        }

        return null;
    }
}
