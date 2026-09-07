using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// <see cref="IOhgActionInvoker"/> over the real <see cref="ShowEngineBridge"/> (Plan 7b Task 2).
/// <see cref="ShowEngineBridge.InvokeAsync"/> already never throws (it catches internally and
/// maps to <see cref="ControlInvokeResult.Fail(string)"/>), so this wrapper's own job is narrow:
/// forward the call, and turn <see cref="ShowEngineBridge.RestartAsync"/>'s
/// <see cref="InvalidOperationException"/> (thrown when the engine was never started) — and any
/// other exception it might throw — into a failed result rather than letting it propagate.
/// </summary>
public sealed class BridgeOhgActionInvoker : IOhgActionInvoker
{
    private readonly ShowEngineBridge _bridge;

    public BridgeOhgActionInvoker(ShowEngineBridge bridge)
    {
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
    }

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken ct = default)
        => _bridge.InvokeAsync(actionId, args, ct);

    public async Task<ControlInvokeResult> RestartEngineAsync(CancellationToken ct = default)
    {
        try
        {
            await _bridge.RestartAsync(ct).ConfigureAwait(false);
            return ControlInvokeResult.Success;
        }
        catch (Exception ex)
        {
            return ControlInvokeResult.Fail(ex.Message);
        }
    }
}
