namespace CoreVideoPro.MediaCore.Services;

public sealed record TwitchCredentials(string ClientId, string AccessToken, string RefreshToken,
    DateTimeOffset ExpiresAt, string UserId = "", string Login = "")
{
    public override string ToString() => "Twitch credentials";
}

public sealed record TwitchDevicePrompt(string UserCode, Uri VerificationUri, DateTimeOffset ExpiresAt);

/// <summary>One review owner's serialized authorization and rotating token pair.</summary>
public sealed class TwitchAuthorization : IDisposable
{
    private readonly TwitchHttpTransport _http;
    private readonly Func<TwitchCredentials?> _read;
    private readonly Action<TwitchCredentials> _save;
    private readonly Action _clear;
    private readonly TimeProvider _clock;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly SemaphoreSlim _rotation = new(1, 1);
    private TwitchCredentials? _current;
    private long _validatedAt;
    internal TwitchHttpTransport Http => _http;

    public static TwitchAuthorization Create(Func<TwitchCredentials?> read, Action<TwitchCredentials> save, Action clear) =>
        new(TwitchHttpTransport.Create(), read, save, clear);

    internal TwitchAuthorization(TwitchHttpTransport http, Func<TwitchCredentials?> read, Action<TwitchCredentials> save,
        Action clear, TimeProvider? clock = null, Func<TimeSpan, CancellationToken, Task>? delay = null)
    {
        (_http, _read, _save, _clear) = (http, read, save, clear);
        _clock = clock ?? TimeProvider.System;
        _delay = delay ?? Task.Delay;
    }

    public static bool IsClientId(string value) => TwitchHttpTransport.ClientId(value);
    public static bool IsChannel(string value) => TwitchHttpTransport.Login(value);
    public void Dispose() => _http.Dispose();

    internal async Task<TwitchCredentials> AuthorizeAsync(string clientId, Action<TwitchDevicePrompt> prompt, CancellationToken cancellation)
    {
        await _rotation.WaitAsync(cancellation).ConfigureAwait(false);
        try
        {
            var stored = await Task.Run(_read, cancellation).ConfigureAwait(false);
            if (clientId.Length == 0) clientId = stored?.ClientId ?? "";
            if (!IsClientId(clientId)) throw new TwitchChatException(TwitchChatFailure.Authorization);
            if (stored?.ClientId == clientId)
            {
                _current = stored;
                try { return await MaintainLockedAsync(force: true, cancellation).ConfigureAwait(false); }
                catch (TwitchChatException error) when (error.Failure == TwitchChatFailure.Authorization)
                {
                    _current = null;
                    _clear();
                }
            }
            var grant = await _http.BeginAsync(clientId, _clock.GetUtcNow(), cancellation).ConfigureAwait(false);
            prompt(grant.Prompt);
            var interval = grant.Interval;
            while (_clock.GetUtcNow() < grant.Prompt.ExpiresAt)
            {
                await _delay(TimeSpan.FromSeconds(interval), cancellation).ConfigureAwait(false);
                if (_clock.GetUtcNow() >= grant.Prompt.ExpiresAt) break;
                try
                {
                    var pair = await _http.PollAsync(clientId, grant.DeviceCode, cancellation).ConfigureAwait(false);
                    var credentials = new TwitchCredentials(clientId, pair.AccessToken, pair.RefreshToken, _clock.GetUtcNow().AddSeconds(pair.ExpiresIn));
                    return await ValidateAndSaveAsync(credentials, cancellation).ConfigureAwait(false);
                }
                catch (TwitchDevicePendingException pending)
                {
                    if (pending.SlowDown) interval = Math.Min(60, interval + 5);
                }
            }
            throw new TwitchChatException(TwitchChatFailure.Authorization);
        }
        finally { _rotation.Release(); }
    }

    internal async Task<TwitchCredentials> MaintainAsync(CancellationToken cancellation, bool force = false)
    {
        await _rotation.WaitAsync(cancellation).ConfigureAwait(false);
        try { return await MaintainLockedAsync(force, cancellation).ConfigureAwait(false); }
        catch (TwitchChatException error) when (error.Failure == TwitchChatFailure.Authorization)
        {
            _current = null;
            _clear();
            throw;
        }
        finally { _rotation.Release(); }
    }

    private async Task<TwitchCredentials> MaintainLockedAsync(bool force, CancellationToken cancellation)
    {
        var current = _current ?? throw new TwitchChatException(TwitchChatFailure.Authorization);
        var now = _clock.GetUtcNow();
        if (current.ExpiresAt - now <= TimeSpan.FromMinutes(1))
        {
            var pair = await _http.RefreshAsync(current, cancellation).ConfigureAwait(false);
            current = current with { AccessToken = pair.AccessToken, RefreshToken = pair.RefreshToken, ExpiresAt = now.AddSeconds(pair.ExpiresIn) };
            // Device-flow refresh tokens are single-use. Commit the complete rotated
            // pair before any subsequent request can try the previous refresh token.
            _save(current);
            _current = current;
            force = true;
        }
        if (force || _clock.GetElapsedTime(_validatedAt) >= TimeSpan.FromHours(1)) return await ValidateAndSaveAsync(current, cancellation).ConfigureAwait(false);
        return current;
    }

    private async Task<TwitchCredentials> ValidateAndSaveAsync(TwitchCredentials credentials, CancellationToken cancellation)
    {
        var identity = await _http.ValidateAsync(credentials, cancellation).ConfigureAwait(false);
        if (credentials.UserId.Length > 0 && credentials.UserId != identity.UserId)
            throw new TwitchChatException(TwitchChatFailure.Authorization);
        var now = _clock.GetUtcNow();
        var validated = credentials with { UserId = identity.UserId, Login = identity.Login, ExpiresAt = now.AddSeconds(identity.ExpiresIn) };
        _save(validated);
        _current = validated;
        _validatedAt = _clock.GetTimestamp();
        return validated;
    }
}
