using System.Text.RegularExpressions;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Line filter applied to raw log tails (launch.log / media-core.log / perf.log /
/// vcam-serve.log) before they are zipped into a support bundle archive.
///
/// The bundle JSON is redacted builder-side (<see cref="SupportBundleBuilder"/>),
/// but the logs are raw process output. The 2026-07-18 audit found no site that
/// deliberately echoes a secret (the zoom engine emits jwt_present/has_user_zak
/// BOOLEANS, ffmpeg's stderr — which would print the rtmp URL incl. stream key —
/// goes to a separate temp file that is NOT collected, and the core's snapshot
/// endpoints go through redactedEndpoint). Two soft vectors remain, so this
/// belt-and-braces pass runs over every collected tail anyway:
///  - JsonRpcServer's parse-failure diagnostic echoes the first 60 chars of a raw
///    command line; configure-outputs command JSON carries streamKey/passphrase.
///  - Any future log line quoting a join/output payload or URL.
///
/// What it scrubs (case-insensitive):
///  - rtmp(s)://host/app/&lt;stream-key-path&gt; → path after the host is redacted
///  - key/token/passphrase/password/secret/pwd/zak/jwt style query or key=value
///    assignments (streamKey=..., access_token=..., pwd=...)
///  - the same names as quoted JSON string fields ("streamKey":"...")
///  - "Bearer &lt;token&gt;" authorization values
///  - bare JWT-shaped tokens (eyJ&lt;header&gt;.&lt;payload&gt;.&lt;sig&gt;)
/// </summary>
public static class SupportBundleLogRedactor
{
    public const string Placeholder = "[redacted]";

    // rtmp://host/anything — everything after the authority can carry the stream
    // key (ffmpeg-style single-URL endpoints append the key as the last path
    // segment), so the whole path is dropped.
    private static readonly Regex RtmpUrlPath = new(
        @"(rtmps?://[^/\s""']+/)\S+",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // streamKey=..., access_token=..., pwd=..., srtPassphrase=... in query strings,
    // key=value logs, or command lines. The value stops at common delimiters.
    private static readonly Regex SecretAssignment = new(
        @"(\w*(?:key|token|passphrase|password|secret|pwd|zak|jwt))=([^&\s""',}\]]+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // "streamKey":"...", "sdkJwt": "...", "userZak":"..." JSON fields echoed raw.
    private static readonly Regex SecretJsonField = new(
        @"""(\w*(?:key|token|passphrase|password|secret|pwd|zak|jwt)\w*)""\s*:\s*""([^""]*)""",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static readonly Regex BearerToken = new(
        @"\bBearer\s+[A-Za-z0-9\-_.=+/]+",
        RegexOptions.Compiled);

    // Bare JWTs (three dot-joined base64url segments starting with eyJ).
    private static readonly Regex JwtShaped = new(
        @"\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{4,}\b",
        RegexOptions.Compiled);

    /// <summary>Redacts secret-shaped content in a chunk of log text.</summary>
    public static string Redact(string text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text;
        }

        text = RtmpUrlPath.Replace(text, m => $"{m.Groups[1].Value}{Placeholder}");
        text = SecretJsonField.Replace(text, m => $"\"{m.Groups[1].Value}\":\"{Placeholder}\"");
        text = SecretAssignment.Replace(text, m => $"{m.Groups[1].Value}={Placeholder}");
        text = BearerToken.Replace(text, $"Bearer {Placeholder}");
        text = JwtShaped.Replace(text, Placeholder);
        return text;
    }
}
