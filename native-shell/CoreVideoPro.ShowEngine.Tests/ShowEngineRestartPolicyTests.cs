using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public sealed class ShowEngineRestartPolicyTests
{
    private static readonly DateTimeOffset T0 = new(2026, 9, 7, 12, 0, 0, TimeSpan.Zero);

    [Fact]
    public void Delays_Escalate_ThenCapAt30s()
    {
        // Default budget is 5 CONSECUTIVE failures: calls 1..5 spend it (1,2,4,8,16s), the 6th is null.
        var policy = new ShowEngineRestartPolicy();

        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(2), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(4), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(8), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(16), policy.NextDelay(T0));
        Assert.Null(policy.NextDelay(T0));
        Assert.Equal(6, policy.ConsecutiveFailures);
    }

    [Fact]
    public void Delays_Escalate_ThenCapAt30s_WithARaisedBudget()
    {
        // Delays keeps repeating its last (30s) entry past index 5, however high the budget goes.
        var policy = new ShowEngineRestartPolicy(maxConsecutiveFailures: 8);

        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(2), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(4), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(8), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(16), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(30), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(30), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(30), policy.NextDelay(T0));
        Assert.Null(policy.NextDelay(T0));
    }

    [Fact]
    public void SixtySecondsOfRunning_ResetsTheBudget()
    {
        var policy = new ShowEngineRestartPolicy();

        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(T0));
        Assert.Equal(TimeSpan.FromSeconds(2), policy.NextDelay(T0));
        Assert.Equal(2, policy.ConsecutiveFailures);

        // Running, but not for long enough yet: the budget does NOT reset.
        policy.RecordRunning(T0);
        policy.RecordHealthy(T0 + TimeSpan.FromSeconds(59));
        Assert.Equal(2, policy.ConsecutiveFailures);
        Assert.Equal(TimeSpan.FromSeconds(4), policy.NextDelay(T0));

        // A fresh running spell that DOES clear 60s resets the counter to zero.
        policy.RecordRunning(T0);
        policy.RecordHealthy(T0 + TimeSpan.FromSeconds(60));
        Assert.Equal(0, policy.ConsecutiveFailures);
        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(T0));
    }

    [Fact]
    public void Reset_ClearsTheBudget()
    {
        var policy = new ShowEngineRestartPolicy();

        policy.NextDelay(T0);
        policy.NextDelay(T0);
        policy.NextDelay(T0);
        Assert.Equal(3, policy.ConsecutiveFailures);

        policy.Reset();

        Assert.Equal(0, policy.ConsecutiveFailures);
        Assert.Equal(TimeSpan.FromSeconds(1), policy.NextDelay(T0));
    }

    [Fact]
    public void RecordHealthy_BeforeAnyRunningSpell_IsANoOp()
    {
        // No RecordRunning has happened since the failure, so RecordHealthy must not reset anything —
        // otherwise a heartbeat racing a crash could forgive a budget that was never actually earned.
        var policy = new ShowEngineRestartPolicy();

        policy.NextDelay(T0);
        policy.RecordHealthy(T0 + TimeSpan.FromSeconds(120));

        Assert.Equal(1, policy.ConsecutiveFailures);
    }
}
