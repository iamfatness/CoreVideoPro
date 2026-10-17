using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

internal sealed class CpuPreviewConsumers
{
    private readonly HashSet<object> _owners = new();
    private readonly object _gate = new();
    internal bool Active { get { lock (_gate) return _owners.Count != 0; } }
    internal event Action? Changed;
    internal void Set(object owner, bool needsCpuPixels)
    {
        bool changed;
        lock (_gate)
        {
            var before = _owners.Count != 0;
            if (needsCpuPixels) _owners.Add(owner); else _owners.Remove(owner);
            changed = before != (_owners.Count != 0);
        }
        if (changed) Changed?.Invoke();
    }
}

/// <summary>GPU monitors do not need the legacy base64 image stream. CPU fallback retains it.</summary>
internal sealed class CpuPreviewDemand : IDisposable
{
    internal static CpuPreviewConsumers Consumers { get; } = new();
    private readonly IMediaCoreBridge _bridge;
    private readonly SourcePreviewDemandController _controller;
    private bool? _lastSent;
    internal CpuPreviewDemand(IMediaCoreBridge bridge)
    {
        _bridge = bridge;
        _controller = new(async enabled =>
        {
            if (!bridge.Running) return;
            await bridge.SyncAsync([new NativeMediaCoreCommand {
                Type = "set-zoom-thumbnail-demand",
                ExtensionData = new() { ["enabled"] = JsonSerializer.SerializeToElement(enabled) }
            }]).ConfigureAwait(false);
            if (_lastSent != enabled)
            {
                LaunchLog.Write($"cpu-preview: Zoom thumbnail demand {(enabled ? "enabled" : "disabled")}; GPU ingestion unchanged");
                _lastSent = enabled;
            }
        });
        Consumers.Changed += Changed;
        bridge.ProfileChanged += ProfileChanged;
        Changed();
        _ = _controller.RefreshAsync();
    }
    private void Changed() => _ = _controller.SetVisibleAsync(Consumers.Active);
    private void ProfileChanged(NativeMediaCoreProfile profile) => _ = _controller.RefreshAsync();
    public void Dispose()
    {
        Consumers.Changed -= Changed;
        _bridge.ProfileChanged -= ProfileChanged;
        _ = _controller.SetEngineAvailableAsync(false);
    }
}
