namespace CoreVideoPro.Control;

/// <summary>The wire type of a control-action parameter. Kept intentionally small so it
/// maps cleanly onto OSC type tags (i/f/s/T/F) and JSON.</summary>
public enum ControlParamType
{
    String,
    Int,
    Double,
    Bool
}

/// <summary>One typed parameter of a control action.</summary>
public sealed record ControlParam(
    string Name,
    ControlParamType Type,
    bool Required = true,
    string? Description = null);

/// <summary>A remotely-invocable operator action, identified by a STABLE dot-namespaced id
/// (e.g. <c>transport.take</c>) that is decoupled from the C# command method name. The id is
/// also the OSC address (with '.'→'/' and a leading '/cvp/'): <c>transport.take</c> ⇄
/// <c>/cvp/transport/take</c>. Params are positional (OSC args / JSON array order).</summary>
public sealed record ControlAction(
    string Id,
    string Title,
    string Description,
    IReadOnlyList<ControlParam> Params)
{
    public ControlAction(string id, string title, string description)
        : this(id, title, description, System.Array.Empty<ControlParam>())
    {
    }
}
