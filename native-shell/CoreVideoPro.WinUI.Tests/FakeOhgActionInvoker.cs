using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Test double for <see cref="IOhgActionInvoker"/> (Plan 7b Task 2). Records every invocation in
/// order and lets a test script the result via <see cref="Handler"/> (default: always
/// <see cref="ControlInvokeResult.Success"/>).
/// </summary>
public sealed class FakeOhgActionInvoker : IOhgActionInvoker
{
    public List<(string ActionId, IReadOnlyList<object?> Args)> Invocations { get; } = new();

    public Func<string, IReadOnlyList<object?>, ControlInvokeResult>? Handler { get; set; }

    public int RestartCalls { get; private set; }

    public ControlInvokeResult RestartResult { get; set; } = ControlInvokeResult.Success;

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken ct = default)
    {
        Invocations.Add((actionId, args));
        var result = Handler?.Invoke(actionId, args) ?? ControlInvokeResult.Success;
        return Task.FromResult(result);
    }

    public Task<ControlInvokeResult> RestartEngineAsync(CancellationToken ct = default)
    {
        RestartCalls++;
        return Task.FromResult(RestartResult);
    }
}
