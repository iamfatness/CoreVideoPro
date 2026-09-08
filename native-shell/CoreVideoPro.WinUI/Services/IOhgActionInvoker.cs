using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using CoreVideoPro.Control;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The seam between OHG UI/control surfaces and the show engine bridge (Plan 7b Task 2).
/// Callers build args with <see cref="OhgActionArgs"/> and invoke through here rather than
/// touching <c>ShowEngineBridge</c> directly, so UI code and tests can both depend on this
/// narrow, fakeable interface.
/// </summary>
public interface IOhgActionInvoker
{
    /// <summary>Never throws. Returns the bridge's <see cref="ControlInvokeResult"/> (success,
    /// or <c>Fail(reason/message)</c>) — an unknown action id, a bad arg, or a down engine all
    /// collapse to a failed result, never a propagated exception.</summary>
    Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken ct = default);

    /// <summary>Restart the show engine. Never throws — see <see cref="BridgeOhgActionInvoker"/>
    /// for how the bridge's <c>InvalidOperationException</c> (never started) is mapped.</summary>
    Task<ControlInvokeResult> RestartEngineAsync(CancellationToken ct = default);
}
