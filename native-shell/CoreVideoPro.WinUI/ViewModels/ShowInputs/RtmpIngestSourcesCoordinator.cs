using System.Collections.ObjectModel;
using System.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels.ShowInputs;

/// <summary>Owns operator RTMP listener intent, persistence, and the native command projection.</summary>
public sealed class RtmpIngestSourcesCoordinator : IDisposable
{
    public const int MaxSources = 8;
    private readonly IRtmpIngestSourceStore _store;
    public ObservableCollection<RtmpIngestSource> Sources { get; } = [];
    public bool PersistenceError { get; private set; }
    public event Action? Changed;

    public RtmpIngestSourcesCoordinator(IRtmpIngestSourceStore store)
    {
        _store = store;
        var loaded = store.Load()
            .Where(source => source.Number is >= 1 and <= MaxSources)
            .DistinctBy(source => source.Number)
            .OrderBy(source => source.Number)
            .Select(source => Create(source.Number, source.Url))
            .ToList();
        foreach (var source in loaded.Count > 0 ? loaded : [Create(1)])
        {
            Sources.Add(source);
            source.PropertyChanged += OnSourceChanged;
        }
    }

    public bool CanAdd => Sources.Count < MaxSources;

    public RtmpIngestSource? Add()
    {
        if (!CanAdd) return null;
        var number = Enumerable.Range(1, MaxSources)
            .First(value => Sources.All(source => source.Number != value));
        var source = Create(number);
        Sources.Add(source);
        source.PropertyChanged += OnSourceChanged;
        SaveAndNotify();
        return source;
    }

    public RtmpIngestSource? Remove(string id)
    {
        if (Sources.Count <= 1) return null;
        var source = Sources.FirstOrDefault(item => item.Id == id);
        if (source is null) return null;
        source.PropertyChanged -= OnSourceChanged;
        Sources.Remove(source);
        SaveAndNotify();
        return source;
    }

    public IReadOnlyList<MediaCoreRtmpIngestSourceWire> BuildWire() => Sources
        .Where(source => RtmpIngestSourcePolicy.IsValid(source.Url))
        // FFmpeg's listener binds a TCP port, regardless of the application or
        // stream path. Two sources on the same port cannot both accept a publisher.
        .DistinctBy(source => RtmpIngestSourcePolicy.ListenerPort(source.Url.Trim()))
        .Select(source => new MediaCoreRtmpIngestSourceWire(
            source.Id, source.DeviceId, source.Name, source.Url.Trim()))
        .ToList();

    public void Dispose()
    {
        foreach (var source in Sources) source.PropertyChanged -= OnSourceChanged;
    }

    private void OnSourceChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName == nameof(RtmpIngestSource.Url)) SaveAndNotify();
    }

    private void SaveAndNotify()
    {
        try
        {
            _store.Save(Sources.Select(source => new RtmpIngestSourceRecord(source.Number, source.Url)).ToList());
            PersistenceError = false;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or System.Security.Cryptography.CryptographicException)
        {
            PersistenceError = true;
        }
        Changed?.Invoke();
    }

    private static RtmpIngestSource Create(int number, string url = "") => new()
    {
        Id = $"rtmp-source-{number:00}", Number = number, Url = url
    };
}
