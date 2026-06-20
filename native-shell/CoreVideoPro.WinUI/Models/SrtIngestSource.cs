namespace CoreVideoPro.WinUI.Models;

public sealed class SrtIngestSource : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
{
    public required string Id { get; init; }
    public required int Number { get; init; }

    private string _mode = "listener";
    public string Mode
    {
        get => _mode;
        set
        {
            if (SetProperty(ref _mode, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    private string _host = "0.0.0.0";
    public string Host
    {
        get => _host;
        set
        {
            if (SetProperty(ref _host, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    private string _port = "10000";
    public string Port
    {
        get => _port;
        set
        {
            if (SetProperty(ref _port, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    private string _latencyMs = "120";
    public string LatencyMs
    {
        get => _latencyMs;
        set
        {
            if (SetProperty(ref _latencyMs, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    private string _streamId = string.Empty;
    public string StreamId
    {
        get => _streamId;
        set
        {
            if (SetProperty(ref _streamId, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    private string _passphrase = string.Empty;
    public string Passphrase
    {
        get => _passphrase;
        set
        {
            if (SetProperty(ref _passphrase, value))
            {
                NotifyDerivedPropertiesChanged();
            }
        }
    }

    public string DeviceId => $"srt-ingest-{Number:00}";

    public string Name => $"SRT {Number}";

    public string Summary =>
        $"{Normalize(Mode, "listener")} {Normalize(Host, "0.0.0.0")}:{Normalize(Port, "10000")} - {Normalize(LatencyMs, "120")} ms latency";

    public string NativeUri => $"srt://{Normalize(Host, "0.0.0.0")}:{Normalize(Port, "10000")}";

    private void NotifyDerivedPropertiesChanged()
    {
        OnPropertyChanged(nameof(Summary));
        OnPropertyChanged(nameof(NativeUri));
    }

    private static string Normalize(string value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();
}
