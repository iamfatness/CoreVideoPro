namespace CoreVideoPro.ShowEngine;

/// <summary>
/// Exponential backoff with a healthy-running reset (spec §6.3), consulted from
/// <see cref="ShowEngineSupervisor"/> on every crash-recovery path — INCLUDING a handshake TIMEOUT
/// (<see cref="ShowEngineSupervisor.AbortStart"/>'s non-terminal branch also calls
/// <see cref="ShowEngineSupervisor.BeginRecovery"/>, which consults <see cref="NextDelay"/> exactly the
/// same as an ordinary post-Running crash) — so a child that never manages to handshake still spends
/// budget entries, same as one that crashes after running fine. Exit 78 is the one path that bypasses
/// this class entirely (terminal, no backoff, no respawn).
/// </summary>
public class ShowEngineRestartPolicy
{
    /// <summary>Backoff ladder in seconds: 1, 2, 4, 8, 16, then 30 repeating for any budget beyond the
    /// sixth consecutive failure.</summary>
    public static readonly TimeSpan[] Delays =
    {
        TimeSpan.FromSeconds(1),
        TimeSpan.FromSeconds(2),
        TimeSpan.FromSeconds(4),
        TimeSpan.FromSeconds(8),
        TimeSpan.FromSeconds(16),
        TimeSpan.FromSeconds(30),
    };

    private readonly int _maxConsecutiveFailures;
    private readonly TimeSpan _healthyResetAfter;
    private readonly object _gate = new();

    private int _consecutiveFailures;

    /// <summary>Set by <see cref="RecordRunning"/>, cleared by <see cref="NextDelay"/> and
    /// <see cref="Reset"/>. <see cref="RecordHealthy"/> is a no-op while this is null — a heartbeat
    /// racing a crash must never forgive a budget the child never actually earned by running.</summary>
    private DateTimeOffset? _runningSince;

    public ShowEngineRestartPolicy(int maxConsecutiveFailures = 5, TimeSpan? healthyResetAfter = null)
    {
        _maxConsecutiveFailures = maxConsecutiveFailures;
        _healthyResetAfter = healthyResetAfter ?? TimeSpan.FromSeconds(60);
    }

    /// <summary>How long to wait before the next respawn, or null to give up (state <c>Failed</c>).</summary>
    public virtual TimeSpan? NextDelay(DateTimeOffset now)
    {
        lock (_gate)
        {
            // The running spell that just ended (if any) is over; only a FRESH RecordRunning after this
            // point can ever earn a healthy-reset again.
            _runningSince = null;
            _consecutiveFailures++;
            if (_consecutiveFailures > _maxConsecutiveFailures) return null;

            var index = Math.Min(_consecutiveFailures - 1, Delays.Length - 1);
            return Delays[index];
        }
    }

    /// <summary>A child reached a successful handshake; the clock for "healthy long enough" starts.</summary>
    public virtual void RecordRunning(DateTimeOffset now)
    {
        lock (_gate) { _runningSince = now; }
    }

    /// <summary>A heartbeat round-tripped; the child is alive at <paramref name="now"/>. Resets the
    /// consecutive-failure counter once the CURRENT running spell has lasted <c>healthyResetAfter</c>.</summary>
    public virtual void RecordHealthy(DateTimeOffset now)
    {
        lock (_gate)
        {
            if (_runningSince is not { } since) return;
            if (now - since >= _healthyResetAfter) _consecutiveFailures = 0;
        }
    }

    public virtual int ConsecutiveFailures { get { lock (_gate) return _consecutiveFailures; } }

    /// <summary>Operator-initiated restart: forget the failure history.</summary>
    public virtual void Reset()
    {
        lock (_gate)
        {
            _consecutiveFailures = 0;
            _runningSince = null;
        }
    }
}
