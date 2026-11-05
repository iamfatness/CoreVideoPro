using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Owns the private social queue and the sole command path from an approved item to air.</summary>
public sealed class GraphicsSocialPlayout
{
    internal const string ClientId = "graphics-social-review";
    internal const string InstancePrefix = "social-review-";
    internal const string TemplateId = "youtube-chat-card";
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    internal static readonly JsonElement CardDefinition = LoadCardDefinition();

    private static JsonElement LoadCardDefinition()
    {
        using var stream = typeof(GraphicsSocialPlayout).Assembly.GetManifestResourceStream("CoreVideoPro.Graphics.YouTubeChatCard.json")
            ?? throw new InvalidOperationException("The bundled YouTube chat template is missing.");
        return JsonSerializer.Deserialize<JsonElement>(stream);
    }

    private readonly object _gate = new();
    private readonly SemaphoreSlim _operations = new(1, 1);
    private readonly GraphicsSocialReview _review;
    private readonly Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> _invoke;
    private readonly Func<NativeGraphicsState?> _observe;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly TimeProvider _clock;
    private Card? _card;
    private sealed record Card(GraphicsSocialItemKey Key, string Epoch, string Id, long Revision = 1);

    internal GraphicsSocialPlayout(Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe, Func<TimeSpan, CancellationToken, Task>? delay = null, TimeProvider? clock = null)
    {
        (_invoke, _observe) = (invoke, observe);
        _delay = delay ?? Task.Delay;
        _clock = clock ?? TimeProvider.System;
        _review = new(_clock.GetUtcNow);
    }

    public IReadOnlyList<GraphicsSocialItem> Items { get { lock (_gate) return _review.Items; } }
    public bool IsConnected { get { lock (_gate) return _review.IsConnected; } }
    public bool OnProgram { get { lock (_gate) return _card is { } card &&
        Current(card.Epoch)?.Instances.Any(i => i.InstanceId == card.Id && GraphicsPlayoutService.IsOnProgram(i)) == true; } }

    public Task<long> ConnectAsync() => ChangeAsync(_review.Connect);
    public Task DisconnectAsync() => ChangeAsync(() => { _review.Disconnect(); return true; });
    public Task ExpireAsync() => ChangeAsync(() => _review.Items);

    internal Task<bool> ApplyAsync(long source, IReadOnlyList<SocialFeedEvent> events) => ChangeAsync(() =>
    {
        foreach (var item in events)
        {
            _review.Apply(source, item);
        }
        return _review.IsCurrent(source);
    });

    internal Task DisconnectAsync(long source) => ChangeAsync(() =>
    {
        if (_review.IsCurrent(source)) _review.Disconnect();
        return true;
    });
    public Task<GraphicsSocialItem?> ReceiveAsync(long source, string id, string author, string message) =>
        ChangeAsync(() => _review.Receive(source, id, author, message));
    public Task WithdrawAsync(long source, string id) => ChangeAsync(() => { _review.Withdraw(source, id); return true; });
    public Task<bool> WithdrawAsync(GraphicsSocialItemKey key) => ChangeAsync(() => _review.Withdraw(key));
    public bool Approve(GraphicsSocialItemKey key) { lock (_gate) return _review.Approve(key); }
    public bool Reject(GraphicsSocialItemKey key) { lock (_gate) return _review.Reject(key); }

    public async Task<NativeGraphicsOperationResult> ShowAsync(GraphicsSocialItemKey key, CancellationToken cancellation = default)
    {
        await _operations.WaitAsync(cancellation);
        Card? attempted = null;
        var shown = false;
        try
        {
            GraphicsSocialItem? item;
            lock (_gate) item = _review.Approved(key);
            if (item is null) return Refuse("Approve this exact message before showing it.");
            if (!await RetireAsync(invalidOnly: false)) return Refuse("The previous social graphic could not be cleared.");
            if (_observe() is not { CoreEpoch.Length: > 0 } state) return Refuse("Native Graphics is unavailable.");
            var epoch = state.CoreEpoch;
            var template = state.Templates.FirstOrDefault(t => t.TemplateId == TemplateId && t.TemplateVersion == 1);
            if (template is null)
            {
                var (installed, refusal) = await GraphicsTemplateInstall.RunAsync(
                    (action, body, ct) => Send(action, epoch, body, ct),
                    () => Current(epoch) ?? throw new InvalidOperationException("Native Graphics restarted."),
                    _delay, () => _clock.GetUtcNow().UtcDateTime, CardDefinition.GetRawText(), "", TemplateId, 1, cancellation);
                if (refusal is not null) return refusal;
                template = installed;
            }
            if (template?.Definition is not { } definition || !JsonElement.DeepEquals(definition, CardDefinition))
                return Refuse("The native social graphic template could not be verified.");

            Task<NativeGraphicsOperationResult> create;
            lock (_gate)
            {
                cancellation.ThrowIfCancellationRequested();
                if (_review.Approved(key) is null) return Refuse("The message changed before Show.");
                _card = attempted = new(key, epoch, InstancePrefix + Guid.NewGuid().ToString("N"));
                create = Send("graphics.instance.create", epoch, new() {
                    ["instanceId"] = attempted.Id, ["scope"] = "global", ["templateId"] = TemplateId,
                    ["templateVersion"] = 1, ["fields"] = new { item.Author, item.Message } }, CancellationToken.None);
            }
            var created = await create;
            if (!created.Accepted) return created;
            var cue = await Instance("graphics.instance.cue", attempted, new() { ["rideTake"] = false });
            if (!cue.Accepted) return cue;
            var started = _clock.GetTimestamp();
            while (true)
            {
                cancellation.ThrowIfCancellationRequested();
                var observed = Current(epoch);
                if (observed is null) return Refuse("Native Graphics restarted.");
                var instance = observed.Instances.FirstOrDefault(i => i.InstanceId == attempted.Id);
                if (instance is { CueRevision: 1, CuePending: false, Revision: 1 }) break;
                if (instance?.CuePreparation == "failed" || _clock.GetElapsedTime(started) >= TimeSpan.FromSeconds(5))
                    return Refuse("The social graphic could not prepare.");
                lock (_gate) if (_review.Approved(key) is null) return Refuse("The message changed before Show.");
                await _delay(TimeSpan.FromMilliseconds(50), cancellation);
            }

            Task<NativeGraphicsOperationResult> submit;
            lock (_gate)
            {
                cancellation.ThrowIfCancellationRequested();
                if (_review.Approved(key) is null || Current(epoch) is null)
                    return Refuse("The message changed before Show.");
                submit = Instance("graphics.instance.trigger", attempted, new() { ["action"] = "show", ["target"] = "cue" });
            }
            var result = await submit;
            shown = result.Accepted;
            return result;
        }
        catch (InvalidOperationException) { return Refuse("The social graphic could not be shown."); }
        catch (OperationCanceledException) { return Refuse("Social Show was cancelled."); }
        finally
        {
            try { if (attempted is not null) await RetireAsync(invalidOnly: shown); }
            finally { _operations.Release(); }
        }
    }

    public async Task<bool> HideAsync()
    {
        await _operations.WaitAsync();
        try { return await RetireAsync(invalidOnly: false); }
        finally { _operations.Release(); }
    }

    private async Task<T> ChangeAsync<T>(Func<T> apply)
    {
        bool retire;
        T value;
        lock (_gate)
        {
            value = apply();
            retire = _card is { } card && _review.Approved(card.Key) is null;
        }
        if (retire)
        {
            await _operations.WaitAsync();
            try { await RetireAsync(invalidOnly: true); }
            finally { _operations.Release(); }
        }
        return value;
    }

    private async Task<bool> RetireAsync(bool invalidOnly)
    {
        Card? card;
        lock (_gate)
        {
            card = _card;
            if (card is null || (invalidOnly && _review.Approved(card.Key) is not null)) return true;
        }
        for (var attempt = 0; attempt < 3 && Current(card.Epoch) is not null; attempt++)
        {
            var cleared = await Instance("graphics.instance.trigger", card, new() { ["action"] = "clear" });
            if (cleared.Accepted || cleared.Reason == "instance-not-found")
            {
                var removed = await Instance("graphics.instance.remove", card, []);
                if (removed.Accepted || removed.Reason == "instance-not-found")
                {
                    lock (_gate) _card = null;
                    return true;
                }
            }
            await _delay(TimeSpan.FromMilliseconds(50), CancellationToken.None);
        }
        if (Current(card.Epoch) is not null) return false;
        lock (_gate) _card = null;
        return true;
    }

    private NativeGraphicsState? Current(string epoch) => _observe() is { } state && state.CoreEpoch == epoch ? state : null;

    private Task<NativeGraphicsOperationResult> Instance(string action, Card card, Dictionary<string, object?> body)
    {
        body["instanceId"] = card.Id;
        body["expectedRevision"] = card.Revision;
        return Send(action, card.Epoch, body, CancellationToken.None);
    }

    private Task<NativeGraphicsOperationResult> Send(string action, string epoch, Dictionary<string, object?> body, CancellationToken cancellation)
    {
        if (Current(epoch) is null) return Task.FromResult(Refuse("Native Graphics restarted."));
        body["schemaVersion"] = 1;
        body["clientId"] = ClientId;
        body.TryAdd("requestId", "social-" + Guid.NewGuid().ToString("N"));
        body["coreEpoch"] = epoch;
        return _invoke(action, JsonSerializer.Serialize(body, Json), cancellation);
    }

    private static NativeGraphicsOperationResult Refuse(string reason) => new() { Reason = reason };
}
