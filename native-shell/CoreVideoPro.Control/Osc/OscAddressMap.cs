namespace CoreVideoPro.Control.Osc;

/// <summary>Maps between stable dot-namespaced action ids and OSC addresses under a common root
/// (default <c>/cvp</c>): <c>transport.take</c> ⇄ <c>/cvp/transport/take</c>. Feedback state fields
/// live under <c>/cvp/state/&lt;field&gt;</c>.</summary>
public sealed class OscAddressMap
{
    public OscAddressMap(string root = "/cvp")
    {
        Root = NormalizeRoot(root);
    }

    public string Root { get; }

    public string StatePrefix => $"{Root}/state/";

    /// <summary>Convert an incoming OSC address to a candidate action id, or null when it is not
    /// under the action root. Does NOT verify the id exists in the registry.</summary>
    public string? AddressToActionId(string address)
    {
        if (string.IsNullOrEmpty(address) || !address.StartsWith(Root + "/", StringComparison.Ordinal))
        {
            return null;
        }

        var tail = address[(Root.Length + 1)..];
        if (tail.Length == 0 || tail.StartsWith("state/", StringComparison.Ordinal))
        {
            return null;  // feedback namespace, not an action
        }

        return tail.Replace('/', '.');
    }

    public string ActionIdToAddress(string actionId) => $"{Root}/{actionId.Replace('.', '/')}";

    public string StateAddress(string field) => $"{StatePrefix}{field}";

    private static string NormalizeRoot(string root)
    {
        var trimmed = (root ?? "/cvp").Trim();
        if (trimmed.Length == 0)
        {
            return "/cvp";
        }

        if (!trimmed.StartsWith('/'))
        {
            trimmed = "/" + trimmed;
        }

        return trimmed.TrimEnd('/');
    }
}
