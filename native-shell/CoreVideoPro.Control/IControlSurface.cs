namespace CoreVideoPro.Control;

/// <summary>Outcome of a control action invocation.</summary>
/// <summary>Outcome of a control action invocation. <paramref name="Data"/> is the JSON text a
/// read action (graphics.playlist.list, graphics.strip) answers with; transports pass it through
/// verbatim under "data" and never parse it.</summary>
public sealed record ControlInvokeResult(bool Ok, string? Error = null, string? PreviewSceneId = null, string? Data = null)
{
    public static ControlInvokeResult Success { get; } = new(true);

    public static ControlInvokeResult Fail(string error) => new(false, error);

    public static ControlInvokeResult AppliedPreview(string previewSceneId) => new(true, null, previewSceneId);

    public static ControlInvokeResult WithData(string json) => new(true, null, null, json);
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
