namespace CoreVideoPro.MediaCore.Services;

/// <summary>One explicitly connected public YouTube chat. Approval state never crosses a lost connection.</summary>
public sealed class YouTubeSocialFeed
{
    public static bool IsVideoId(string value) => YouTubeChatTransport.IsVideoId(value);
    public static bool IsApiKey(string value) => YouTubeChatTransport.IsApiKey(value);

    public static async Task RunAsync(string videoId, string apiKey, GraphicsSocialPlayout playout,
        Action<string> report, CancellationToken cancellation)
    {
        using var transport = YouTubeChatTransport.Create();
        await RunAsync(videoId, apiKey, playout, transport, report, Task.Delay, cancellation).ConfigureAwait(false);
    }

    internal static async Task RunAsync(string videoId, string apiKey, GraphicsSocialPlayout playout,
        YouTubeChatTransport transport, Action<string> report, Func<TimeSpan, CancellationToken, Task> delay,
        CancellationToken cancellation)
    {
        var retries = 0;
        long source = 0;
        try
        {
            while (true)
            {
                cancellation.ThrowIfCancellationRequested();
                source = await playout.ConnectAsync().ConfigureAwait(false);
                report("Connecting to YouTube…");
                try
                {
                    var chat = await transport.ResolveChatAsync(videoId, apiKey, cancellation).ConfigureAwait(false);
                    string? cursor = null;
                    while (true)
                    {
                        cancellation.ThrowIfCancellationRequested();
                        var page = await transport.ReadPageAsync(chat, cursor, apiKey, cancellation).ConfigureAwait(false);
                        cancellation.ThrowIfCancellationRequested();
                        if (!await playout.ApplyAsync(source, page.Events).ConfigureAwait(false))
                        {
                            report("Review queue reset. Reconnect to start a new session.");
                            return;
                        }
                        retries = 0;
                        cursor = page.Cursor;
                        report("Connected to YouTube live chat.");
                        await delay(page.Interval, cancellation).ConfigureAwait(false);
                    }
                }
                catch (YouTubeChatException error) when (error.Failure == YouTubeChatFailure.Retry && retries < 3)
                {
                    await playout.DisconnectAsync(source).ConfigureAwait(false);
                    var wait = TimeSpan.FromSeconds(30 * (1 << retries++));
                    report("YouTube chat disconnected. Review decisions cleared. Reconnecting…");
                    await delay(wait, cancellation).ConfigureAwait(false);
                }
            }
        }
        catch (YouTubeChatException error) { report(error.Message); }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        finally { if (source != 0) await playout.DisconnectAsync(source).ConfigureAwait(false); }
    }
}
