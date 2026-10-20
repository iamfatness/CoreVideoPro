using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class MediaCoreSupervisor
{
    private readonly EngineHostClient? _hostClient;
    private int _hostBufferFrames = 3;
    private readonly object _hostResourcesGate = new();
    private readonly Dictionary<string, string> _hostResources = new();
    public string? HostedEpoch => _hostClient?.Epoch;
    public bool Hosted => _hostClient is not null;
    public bool HostedReadOnly => _hostClient?.ReadOnly == true;
    public int HostedProcessId => _hostClient?.CorePid ?? _process?.Id ?? 0;
    public Task HostedControlAsync(string operation, CancellationToken cancellationToken) =>
        (_hostClient ?? throw new InvalidOperationException("Not a hosted connection.")).ControlAsync(operation, cancellationToken);

    // Called only by the local EngineHost owner, not a remote observer.
    public Task<JsonDocument> ForwardHostedCommandAsync(Dictionary<string, object?> command, CancellationToken cancellationToken)
    {
        command["id"] = NextId();
        return SendAsync(command, cancellationToken, timeoutMs: 60000);
    }

    private void CacheHostedResource(string line)
    {
        // Only bounded structural resource descriptors, never base64 CPU frames.
        if (line.Length > 64 * 1024) return;
        try
        {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (root.TryGetProperty("id", out _)) return;
            var key = EngineHostWire.ResourceKey(root);
            if (key is null) return;
            lock (_hostResourcesGate)
            {
                if (_hostResources.Count >= 32 && !_hostResources.ContainsKey(key)) return;
                _hostResources[key] = line;
            }
        }
        catch (JsonException) { }
    }

    public string[] HostedResourceEvents()
    {
        lock (_hostResourcesGate) return _hostResources.Values.ToArray();
    }

    private void PublishHostedResource(string line)
    {
        if (CoreProtocolParser.TryParseProgramSharedTextureEvent(line) is { } program)
            ProgramSharedTextureReceived?.Invoke(program.Texture);
        else if (CoreProtocolParser.TryParsePreviewSharedTextureEvent(line) is { } preview)
            PreviewSharedTextureReceived?.Invoke(preview.Texture);
        else if (CoreProtocolParser.TryParseParticipantSharedTextureEvent(line) is { } participant)
            ParticipantSharedTextureReceived?.Invoke(participant.Texture);
        else if (CoreProtocolParser.TryParseMultiviewSharedTextureEvent(line) is { } multiview)
            MultiviewSharedTextureReceived?.Invoke(multiview.Multiview);
    }
}
