namespace CoreVideoPro.MediaCore.Services;

/// <summary>One selected channel; every connection gap starts a new private review.</summary>
public static class TwitchSocialFeed
{
    public static Task RunAsync(string channel, string clientId, TwitchAuthorization authorization,
        GraphicsSocialPlayout playout, Action<string> report, Action<TwitchDevicePrompt> prompt, CancellationToken cancellation) =>
        RunAsync(channel, clientId, authorization, playout, TwitchChatSocket.ConnectAsync, report, prompt, Task.Delay, cancellation);

    internal static async Task RunAsync(string channel, string clientId, TwitchAuthorization authorization,
        GraphicsSocialPlayout playout, Func<CancellationToken, Task<ITwitchChatSocket>> connect,
        Action<string> report, Action<TwitchDevicePrompt> prompt, Func<TimeSpan, CancellationToken, Task> delay,
        CancellationToken cancellation)
    {
        long source = 0;
        try
        {
            await playout.DisconnectAsync().ConfigureAwait(false);
            if (!TwitchAuthorization.IsChannel(channel)) throw TwitchJson.Invalid();
            report("Authorizing Twitch chat…");
            var credentials = await authorization.AuthorizeAsync(clientId, prompt, cancellation).ConfigureAwait(false);
            for (var retry = 0; ;)
            {
                cancellation.ThrowIfCancellationRequested();
                try
                {
                    if (retry > 0) credentials = await authorization.MaintainAsync(cancellation, force: true).ConfigureAwait(false);
                    report("Connecting to Twitch chat…");
                    var broadcaster = await authorization.Http.ChannelAsync(channel, credentials, cancellation).ConfigureAwait(false);
                    using var socket = await connect(cancellation).ConfigureAwait(false);
                    var welcome = await socket.ReadAsync(TimeSpan.FromSeconds(10), cancellation).ConfigureAwait(false);
                    var session = TwitchEventSubSession.Welcome(welcome, broadcaster, credentials.UserId);
                    foreach (var type in TwitchEventSubSession.RequiredTypes)
                        await authorization.Http.SubscribeAsync(session, type, broadcaster, credentials, cancellation).ConfigureAwait(false);
                    source = await playout.ConnectAsync().ConfigureAwait(false);
                    report("Connected to Twitch live chat: " + channel + ".");
                    using var connected = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
                    var reader = ReadAsync(socket, session, playout, source, connected.Token);
                    var authority = MaintainAsync(authorization, delay, connected.Token);
                    try { await await Task.WhenAny(reader, authority).ConfigureAwait(false); }
                    finally
                    {
                        connected.Cancel();
                        // Invalidate approval before waiting for pending HTTP or socket cleanup.
                        await playout.DisconnectAsync(source).ConfigureAwait(false);
                        socket.Dispose();
                        try { await Task.WhenAll(reader, authority).ConfigureAwait(false); }
                        catch (Exception error) when (error is OperationCanceledException or TwitchChatException) { }
                    }
                }
                catch (TwitchChatException error) when (error.Failure == TwitchChatFailure.Retry && retry < 3)
                {
                    if (source != 0) await playout.DisconnectAsync(source).ConfigureAwait(false);
                    report("Twitch chat disconnected. Review decisions cleared. Reconnecting…");
                    await delay(TimeSpan.FromSeconds(1 << retry++), cancellation).ConfigureAwait(false);
                }
            }
        }
        catch (TwitchChatException error) { report(error.Message); }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        finally { if (source != 0) await playout.DisconnectAsync(source).ConfigureAwait(false); }
    }

    private static async Task ReadAsync(ITwitchChatSocket socket, TwitchEventSubSession session,
        GraphicsSocialPlayout playout, long source, CancellationToken cancellation)
    {
        while (true)
        {
            var message = session.Read(await socket.ReadAsync(session.Keepalive, cancellation).ConfigureAwait(false));
            if (message.Kind == TwitchSocketEventKind.Revoked) throw new TwitchChatException(TwitchChatFailure.Revoked);
            // A planned move uses a fresh canonical connection too; no unproven continuity.
            if (message.Kind == TwitchSocketEventKind.Reconnect) throw new TwitchChatException(TwitchChatFailure.Retry);
            if (message.Kind == TwitchSocketEventKind.Notification &&
                !await playout.ApplyAsync(source, message.Events).ConfigureAwait(false)) throw TwitchJson.Invalid();
        }
    }

    private static async Task MaintainAsync(TwitchAuthorization authorization,
        Func<TimeSpan, CancellationToken, Task> delay, CancellationToken cancellation)
    {
        while (true)
        {
            await delay(TimeSpan.FromSeconds(30), cancellation).ConfigureAwait(false);
            await authorization.MaintainAsync(cancellation).ConfigureAwait(false);
        }
    }
}
