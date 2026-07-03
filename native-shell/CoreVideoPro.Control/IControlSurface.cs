namespace CoreVideoPro.Control;

/// <summary>Outcome of a control action invocation.</summary>
public sealed record ControlInvokeResult(bool Ok, string? Error = null)
{
    public static ControlInvokeResult Success { get; } = new(true);

    public static ControlInvokeResult Fail(string error) => new(false, error);
}

/// <summary>The seam between the transport-agnostic control layer (registry + OSC/HTTP) and the
/// operator app. The WinUI shell implements this over <c>StudioViewModel</c>, marshaling every
/// invocation onto the UI thread; the control library never references WinUI, so it is fully
/// unit-testable against a fake surface.</summary>
public interface IControlSurface
{
    /// <summary>Invoke an action id with already-bound, schema-validated positional args
    /// (see <see cref="ControlActionRegistry.TryBind"/>). Implementations dispatch to the UI
    /// thread and translate the id into a command execution or property set.</summary>
    Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default);

    /// <summary>The current feedback state for control-surface button lighting.</summary>
    ControlState GetState();

    /// <summary>Raised (coalesced) when the feedback state changes, so transports can push
    /// updated feedback without polling.</summary>
    event EventHandler<ControlState>? StateChanged;
}
