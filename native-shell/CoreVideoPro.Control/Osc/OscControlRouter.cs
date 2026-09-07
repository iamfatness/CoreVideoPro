using System.Net;

namespace CoreVideoPro.Control.Osc;

/// <summary>Transport-independent core of the OSC control server: turns a decoded OSC message into
/// a validated action invocation on the <see cref="IControlSurface"/>. Unit-testable without a
/// real socket. The UDP <see cref="OscControlServer"/> is a thin wrapper over this.</summary>
public sealed class OscControlRouter
{
    private readonly IControlSurface _surface;
    private readonly OscAddressMap _addressMap;
    private readonly ControlCatalog _catalog;

    public OscControlRouter(IControlSurface surface, OscAddressMap? addressMap = null, ControlCatalog? catalog = null)
    {
        _surface = surface;
        _addressMap = addressMap ?? new OscAddressMap();
        _catalog = catalog ?? ControlCatalog.StaticOnly;
    }

    /// <summary>Route one OSC message. Returns null when the address is not a known action (so the
    /// caller can ignore unrelated traffic), otherwise the invocation result. <paramref name="sender"/>
    /// is the UDP sender endpoint (null for in-process/unit-test callers, treated as loopback); a
    /// LoopbackOnly-exposed action from a non-loopback sender is refused BEFORE bind, and the
    /// surface is never invoked.</summary>
    public async Task<ControlInvokeResult?> RouteAsync(OscMessage message, IPEndPoint? sender = null, CancellationToken cancellationToken = default)
    {
        var actionId = _addressMap.AddressToActionId(message.Address);
        if (actionId is null || !_catalog.Contains(actionId))
        {
            return null;
        }

        if (_catalog.ExposureOf(actionId) == OscExposure.LoopbackOnly && sender is not null && !IPAddress.IsLoopback(sender.Address))
        {
            return ControlInvokeResult.Fail($"'{actionId}' is not exposed to LAN OSC senders (set COREVIDEO_OSC_OHG_LAN=1)");
        }

        if (!_catalog.TryBind(actionId, message.Args, out var bound, out var error))
        {
            return ControlInvokeResult.Fail(error!);
        }

        return await _surface.InvokeAsync(actionId, bound, cancellationToken).ConfigureAwait(false);
    }
}
