namespace CoreVideoPro.ShowEngine;

/// <summary>
/// STUB (Task 6). The real backoff — 1, 2, 4, 8, 16, 30, 30… seconds, the consecutive-failure counter
/// reset after 60 s of Running, and <c>Failed</c> after 5 consecutive failures (spec §6.3) — is Task 7,
/// which replaces the BODY of these four members and tests them. The surface is complete and already
/// called from the right places in <see cref="ShowEngineSupervisor"/>:
/// <see cref="RecordRunning"/> after each successful handshake, <see cref="RecordHealthy"/> from each
/// successful heartbeat, <see cref="NextDelay"/> on every crash, <see cref="Reset"/> on an operator
/// restart. Until Task 7 lands the supervisor therefore respawns immediately and forever, which is
/// exactly what Task 6's tests assert against.
/// </summary>
public class ShowEngineRestartPolicy
{
    /// <summary>How long to wait before the next respawn, or null to give up (state <c>Failed</c>).</summary>
    public virtual TimeSpan? NextDelay(DateTimeOffset now) => TimeSpan.Zero;

    /// <summary>A child reached a successful handshake; the clock for "healthy long enough" starts.</summary>
    public virtual void RecordRunning(DateTimeOffset now)
    {
    }

    /// <summary>A heartbeat round-tripped; the child is alive at <paramref name="now"/>.</summary>
    public virtual void RecordHealthy(DateTimeOffset now)
    {
    }

    /// <summary>Operator-initiated restart: forget the failure history.</summary>
    public virtual void Reset()
    {
    }

    public virtual int ConsecutiveFailures => 0;
}
