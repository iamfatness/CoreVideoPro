namespace CoreVideoPro.Control.Osc;

/// <summary>Transport-independent core of the OSC control server: turns a decoded OSC message into
/// a validated action invocation on the <see cref="IControlSurface"/>. Unit-testable without a
/// real socket. The UDP <see cref="OscControlServer"/> is a thin wrapper over this.</summary>
public sealed class OscControlRouter
{
    private readonly IControlSurface _surface;
    private readonly OscAddressMap _addressMap;

    public OscControlRouter(IControlSurface surface, OscAddressMap? addressMap = null)
    {
        _surface = surface;
        _addressMap = addressMap ?? new OscAddressMap();
    }

    /// <summary>Route one OSC message. Returns null when the address is not a known action (so the
    /// caller can ignore unrelated traffic), otherwise the invocation result.</summary>
    public async Task<ControlInvokeResult?> RouteAsync(OscMessage message, CancellationToken cancellationToken = default)
    {
        var actionId = _addressMap.AddressToActionId(message.Address);
        if (actionId is null || !ControlActionRegistry.Contains(actionId))
        {
            return null;
        }

        if (!ControlActionRegistry.TryBind(actionId, message.Args, out var bound, out var error))
        {
            return ControlInvokeResult.Fail(error!);
        }

        return await _surface.InvokeAsync(actionId, bound, cancellationToken).ConfigureAwait(false);
    }
}
