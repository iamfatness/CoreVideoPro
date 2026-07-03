using CoreVideoPro.Control;

namespace CoreVideoPro.Control.Tests;

/// <summary>Records invocations so tests can assert routing/binding without a live UI.</summary>
public sealed class FakeControlSurface : IControlSurface
{
    public List<(string ActionId, IReadOnlyList<object?> Args)> Invocations { get; } = new();

    public ControlState State { get; set; } = ControlState.Empty;

    public Func<string, IReadOnlyList<object?>, ControlInvokeResult>? Handler { get; set; }

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default)
    {
        Invocations.Add((actionId, args));
        var result = Handler?.Invoke(actionId, args) ?? ControlInvokeResult.Success;
        return Task.FromResult(result);
    }

    public ControlState GetState() => State;

    public event EventHandler<ControlState>? StateChanged;

    public void RaiseStateChanged(ControlState state)
    {
        State = state;
        StateChanged?.Invoke(this, state);
    }
}
