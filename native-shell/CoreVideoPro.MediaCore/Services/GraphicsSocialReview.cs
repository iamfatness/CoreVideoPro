using System.Text;

namespace CoreVideoPro.MediaCore.Services;

public enum GraphicsSocialDecision { Pending, Approved, Rejected }

public readonly record struct GraphicsSocialItemKey(long Source, string Id, long Revision);

public sealed record GraphicsSocialItem(
    GraphicsSocialItemKey Key, string Author, string Message, DateTimeOffset ReceivedAt,
    GraphicsSocialDecision Decision)
{
    internal string AuthorId { get; init; } = "";
    internal long Order { get; init; }
}

/// <summary>Session-only review state. Its console owner serializes callbacks, decisions and Show submission.</summary>
public sealed class GraphicsSocialReview
{
    private readonly Func<DateTimeOffset> _utcNow;
    private readonly int _capacity;
    private readonly int _removalCapacity;
    private readonly TimeSpan _lifetime;
    private readonly Dictionary<string, GraphicsSocialItem> _items = new(StringComparer.Ordinal);
    private readonly HashSet<string> _removed = new(StringComparer.Ordinal);
    private readonly HashSet<string> _banned = new(StringComparer.Ordinal);
    private readonly Dictionary<string, long> _authorClears = new(StringComparer.Ordinal);
    private long _roomClear;
    private long _source;
    private long _revision;

    public GraphicsSocialReview(Func<DateTimeOffset>? utcNow = null, int capacity = 200,
        int removalCapacity = 4096, TimeSpan? lifetime = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(capacity);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(removalCapacity);
        _lifetime = lifetime ?? TimeSpan.FromMinutes(15);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(_lifetime, TimeSpan.Zero);
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
        _capacity = capacity;
        _removalCapacity = removalCapacity;
    }

    public bool IsConnected { get; private set; }

    public IReadOnlyList<GraphicsSocialItem> Items
    {
        get
        {
            Expire(_utcNow());
            return _items.Values.OrderByDescending(item => item.Key.Revision).ToArray();
        }
    }

    public long Connect()
    {
        Disconnect();
        IsConnected = true;
        return ++_source;
    }

    public void Disconnect()
    {
        IsConnected = false;
        _items.Clear();
        _removed.Clear();
        _banned.Clear();
        _authorClears.Clear();
        _roomClear = 0;
        _revision = 0;
    }

    public GraphicsSocialItem? Receive(long source, string id, string author, string message, string authorId = "", long order = 0)
    {
        var now = _utcNow();
        Expire(now);
        if (!IsCurrent(source) || !ValidId(id) || _removed.Contains(id) || _banned.Contains(authorId) ||
            (_roomClear > 0 && order <= _roomClear) ||
            (_authorClears.TryGetValue(authorId, out var clearedAt) && order <= clearedAt))
            return null;

        var current = _items.GetValueOrDefault(id);
        var normalizedAuthor = Normalize(author, 80);
        var normalizedMessage = Normalize(message, 192);
        if (normalizedAuthor is null || normalizedMessage is null)
        {
            if (current is not null)
                Remove(id);
            return null;
        }

        if (current is not null && current.Author == normalizedAuthor && current.Message == normalizedMessage && current.AuthorId == authorId)
            return current;
        if (current is null && _items.Count >= _capacity)
            return null;

        var item = new GraphicsSocialItem(new(source, id, ++_revision),
            normalizedAuthor, normalizedMessage, current?.ReceivedAt ?? now, GraphicsSocialDecision.Pending) { AuthorId = authorId, Order = order };
        _items[id] = item;
        return item;
    }

    public void Withdraw(long source, string id)
    {
        if (IsCurrent(source) && ValidId(id))
            Remove(id);
    }

    internal void Apply(long source, SocialFeedEvent item)
    {
        switch (item.Kind)
        {
            case SocialFeedEventKind.Text: Receive(source, item.Id, item.Author, item.Text, item.AuthorId, item.Order); break;
            case SocialFeedEventKind.Remove: Withdraw(source, item.Id); break;
            case SocialFeedEventKind.Ban: Ban(source, item.AuthorId); break;
            case SocialFeedEventKind.ClearUser: ClearBefore(source, item.Order, item.AuthorId); break;
            case SocialFeedEventKind.ClearRoom: ClearBefore(source, item.Order, null); break;
        }
    }

    private void ClearBefore(long source, long order, string? authorId)
    {
        if (!IsCurrent(source)) return;
        if (order <= 0 || (authorId is not null && !ValidId(authorId)))
        {
            Disconnect();
            return;
        }
        if (authorId is null) _roomClear = Math.Max(_roomClear, order);
        else _authorClears[authorId] = Math.Max(_authorClears.GetValueOrDefault(authorId), order);
        if (_removed.Count + _banned.Count + _authorClears.Count > _removalCapacity)
        {
            Disconnect();
            return;
        }
        foreach (var item in _items.Values.Where(item => item.Order <= order &&
            (authorId is null || item.AuthorId == authorId)).ToArray())
        {
            Remove(item.Key.Id);
            if (!IsConnected) break;
        }
    }

    internal void Ban(long source, string authorId)
    {
        if (!IsCurrent(source) || !ValidId(authorId)) return;
        _banned.Add(authorId);
        if (_banned.Count + _removed.Count + _authorClears.Count > _removalCapacity)
        {
            Disconnect();
            return;
        }
        foreach (var item in _items.Values.Where(item => item.AuthorId == authorId).ToArray())
        {
            Remove(item.Key.Id);
            if (!IsConnected) break;
        }
    }

    internal bool Withdraw(GraphicsSocialItemKey key)
    {
        if (Find(key) is null) return false;
        Remove(key.Id);
        return true;
    }

    public bool Approve(GraphicsSocialItemKey key) => Decide(key, GraphicsSocialDecision.Approved);

    public bool Reject(GraphicsSocialItemKey key) => Decide(key, GraphicsSocialDecision.Rejected);

    public GraphicsSocialItem? Approved(GraphicsSocialItemKey key) =>
        Find(key) is { Decision: GraphicsSocialDecision.Approved } item ? item : null;

    private bool Decide(GraphicsSocialItemKey key, GraphicsSocialDecision decision)
    {
        if (Find(key) is not { Decision: GraphicsSocialDecision.Pending } item)
            return false;
        _items[key.Id] = item with { Decision = decision };
        return true;
    }

    private GraphicsSocialItem? Find(GraphicsSocialItemKey key)
    {
        Expire(_utcNow());
        return IsCurrent(key.Source) && ValidId(key.Id) &&
            _items.TryGetValue(key.Id, out var item) && item.Key == key ? item : null;
    }

    internal bool IsCurrent(long source) => IsConnected && source == _source;

    private void Expire(DateTimeOffset now)
    {
        foreach (var item in _items.Values.Where(item => now - item.ReceivedAt >= _lifetime).ToArray())
        {
            Remove(item.Key.Id);
            if (!IsConnected)
                break;
        }
    }

    private void Remove(string id)
    {
        _items.Remove(id);
        _removed.Add(id);
        if (_removed.Count + _banned.Count + _authorClears.Count > _removalCapacity)
            Disconnect();
    }

    private static bool ValidId(string? id) =>
        id is { Length: > 0 and <= 256 } && id.All(c => c is >= '!' and <= '~');

    private static string? Normalize(string text, int limit)
    {
        if (text.Length > limit || text.Any(char.IsControl))
            return null;

        try
        {
            var normalized = text.Trim().Normalize(NormalizationForm.FormC);
            return normalized.Length > 0 && Encoding.UTF8.GetByteCount(normalized) <= limit ? normalized : null;
        }
        catch (ArgumentException)
        {
            return null;
        }
    }
}
