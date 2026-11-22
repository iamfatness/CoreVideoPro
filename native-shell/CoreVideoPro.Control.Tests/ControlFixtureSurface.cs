using CoreVideoPro.Control;

namespace CoreVideoPro.Control.Tests;

/// <summary>The fake surface the shared fixtures (contracts/control.fixtures.json) run against: it
/// answers every invoke with <see cref="Reply"/>, records what it received, and serves
/// <see cref="Observation"/> (or throws <see cref="Throw"/>) as the core snapshot. The Mac runner
/// uses its own FakeSurface the same way.</summary>
public sealed class ControlFixtureSurface : IControlSurface, INativeSnapshotObserver
{
    public ControlInvokeResult Reply { get; set; } = ControlInvokeResult.Success;

    public List<(string ActionId, IReadOnlyList<object?> Args)> Invocations { get; } = new();

    public NativeSnapshotObservation Observation { get; set; } = NativeSnapshotObservation.Unavailable("no snapshot");

    /// <summary>When set, GetNativeSnapshot throws with this message (the observer-failed case).</summary>
    public string? Throw { get; set; }

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default)
    {
        Invocations.Add((actionId, args));
        return Task.FromResult(Reply);
    }

    public ControlState GetState() => ControlState.Empty;

    public NativeSnapshotObservation GetNativeSnapshot() =>
        Throw is not null ? throw new InvalidOperationException(Throw) : Observation;

    public event EventHandler<ControlState>? StateChanged { add { } remove { } }
}
