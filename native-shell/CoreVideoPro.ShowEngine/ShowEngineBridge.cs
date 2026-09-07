using CoreVideoPro.Control;

namespace CoreVideoPro.ShowEngine;

/// <summary>
/// Adapts one <see cref="ShowEngineSupervisor"/> onto <see cref="IControlActionProvider"/> — the
/// show engine's action manifest becomes remotely-invocable <see cref="ControlAction"/>s — and gives
/// the WinUI shell (Task 10's host adapter) a small, generation-agnostic surface: the latest
/// snapshot, host commands, health, and log lines, plus the three "tell the engine what the shell
/// already knows" publishers (roster / active speaker / capacity) that must be RE-SENT to every new
/// engine generation, because the engine starts each process with none of that state.
///
/// <para><b>Threading — same contract as <see cref="ShowEngineSupervisor"/>.</b> Every supervisor
/// event this class subscribes to fires on the supervisor's reader thread, and this class does no
/// marshaling of its own — it simply re-raises on whatever thread it was called on. CONSUMERS MUST
/// MARSHAL, exactly as the supervisor's own doc comment says.</para>
///
/// <para><b>Re-arm.</b> On every successful handshake (initial start AND every respawn) the bridge
/// re-sends, in order, the last published capacity (if any), the last published roster (if any), and
/// the last published active speaker (if one was ever set to non-null) — fire-and-forget, logging a
/// failure rather than throwing. A fresh engine generation has forgotten all three.</para>
/// </summary>
public sealed class ShowEngineBridge : IControlActionProvider, IDisposable
{
    private readonly ShowEngineSupervisor _supervisor;
    private readonly object _gate = new();

    private IReadOnlyList<ControlAction> _actions = Array.Empty<ControlAction>();
    private IReadOnlyList<string> _feedbackFieldTemplates = Array.Empty<string>();

    /// <summary>The last successfully-registered raw manifest, used only to detect drift on the NEXT
    /// handshake. Left untouched by a handshake whose manifest failed to map (that generation never
    /// registered anything, so it must not become the drift baseline either).</summary>
    private IReadOnlyList<ShowEngineActionDefinition>? _lastActionDefs;

    private ShowEngineSnapshot? _latest;
    private ShowEngineSpawnRequest? _lastRequest;

    private int _lastCapacity;
    private bool _capacityEverPublished;
    private IReadOnlyList<ShowEngineParticipant> _lastRoster = Array.Empty<ShowEngineParticipant>();
    private bool _rosterEverPublished;
    private string? _lastActiveSpeaker;

    public ShowEngineBridge(ShowEngineSupervisor supervisor, OscExposure oscExposure)
    {
        _supervisor = supervisor ?? throw new ArgumentNullException(nameof(supervisor));
        DefaultOscExposure = oscExposure;

        _supervisor.HealthChanged += OnHealthChanged;
        _supervisor.Handshaken += OnHandshaken;
        _supervisor.SnapshotReceived += OnSnapshotReceived;
        _supervisor.HostCommandReceived += OnHostCommandReceived;
        _supervisor.LogReceived += OnLogReceived;
    }

    // ------------------------------------------------------------------ IControlActionProvider

    public string Id => "show-engine";

    public OscExposure DefaultOscExposure { get; }

    public IReadOnlyList<ControlAction> Actions { get { lock (_gate) return _actions; } }

    public IReadOnlyList<string> FeedbackFieldTemplates { get { lock (_gate) return _feedbackFieldTemplates; } }

    public event EventHandler? ActionsChanged;

    // ------------------------------------------------------------------ bridge surface

    public ShowEngineSnapshot? Latest { get { lock (_gate) return _latest; } }

    public event EventHandler<ShowEngineSnapshot>? SnapshotChanged;

    public event EventHandler<ShowEngineHostCommand>? HostCommand;

    /// <summary>Mirrors the supervisor's own <see cref="ShowEngineSupervisor.Health"/> directly — the
    /// bridge keeps no separate copy, so it can never disagree with it.</summary>
    public ShowEngineHealth Health => _supervisor.Health;

    public event EventHandler<ShowEngineHealth>? HealthChanged;

    public event EventHandler<ShowEngineLogLine>? Log;

    /// <summary>Invoke one show-engine action. Not Running fails immediately, without sending
    /// anything. A transport exception (including a request timeout) is reported through the same
    /// <see cref="ControlInvokeResult.Fail"/> path as an engine-side refusal/error, never thrown.</summary>
    public async Task<ControlInvokeResult> InvokeAsync(
        string actionId, IReadOnlyList<object?> boundArgs, CancellationToken ct)
    {
        var health = _supervisor.Health;
        if (health.State != ShowEngineState.Running)
        {
            return ControlInvokeResult.Fail($"OHG show engine is {health.State.ToString().ToLowerInvariant()}");
        }

        try
        {
            using var response = await _supervisor
                .SendAsync("invoke", new { action = actionId, args = boundArgs }, ct)
                .ConfigureAwait(false);

            var result = ShowEngineProtocol.ParseActionResult(response.RootElement);
            return result.Kind switch
            {
                "ok" => ControlInvokeResult.Success,
                "refused" => ControlInvokeResult.Fail(result.Reason ?? "show engine refused the action"),
                _ => ControlInvokeResult.Fail(result.Message ?? "show engine action failed"),
            };
        }
        catch (Exception ex)
        {
            return ControlInvokeResult.Fail(ex.Message);
        }
    }

    /// <summary>Send the current Zoom roster as a <c>zoomEvent {kind:"roster"}</c>. Fire-and-forget;
    /// remembered so it is re-sent to the next engine generation.</summary>
    public void PublishRoster(IReadOnlyList<ShowEngineParticipant> roster)
    {
        lock (_gate)
        {
            _lastRoster = roster;
            _rosterEverPublished = true;
        }

        SendRoster(roster);
    }

    /// <summary>Send an <c>activeSpeaker</c> request — but ONLY when the id actually changed since the
    /// last publish. <c>null</c> clears the remembered speaker and sends NOTHING: the engine protocol
    /// has no "no one is speaking" event, so the only thing publishable is the next real speaker.
    /// Recorded regardless, so re-arm reflects the true last-known state.</summary>
    public void PublishActiveSpeaker(string? participantId)
    {
        bool changed;
        lock (_gate)
        {
            changed = !string.Equals(_lastActiveSpeaker, participantId, StringComparison.Ordinal);
            _lastActiveSpeaker = participantId;
        }

        if (!changed || participantId is null) return;
        SendActiveSpeaker(participantId);
    }

    /// <summary>Send a <c>capacity</c> request. Fire-and-forget; remembered for re-arm.</summary>
    public void PublishCapacity(int capacity)
    {
        lock (_gate)
        {
            _lastCapacity = capacity;
            _capacityEverPublished = true;
        }

        SendCapacity(capacity);
    }

    public Task StartAsync(ShowEngineSpawnRequest request, CancellationToken ct)
    {
        lock (_gate) _lastRequest = request;
        return _supervisor.StartAsync(request, ct);
    }

    public Task StopAsync() => _supervisor.StopAsync();

    /// <summary>Restart with the LAST spawn request. Throws <see cref="InvalidOperationException"/> if
    /// <see cref="StartAsync"/> was never called.</summary>
    public Task RestartAsync(CancellationToken ct)
    {
        ShowEngineSpawnRequest request;
        lock (_gate)
        {
            if (_lastRequest is null)
            {
                throw new InvalidOperationException("OHG show engine was never started - nothing to restart.");
            }

            request = _lastRequest;
        }

        return _supervisor.RestartAsync(request, ct);
    }

    /// <summary>Pure mapping from the engine's wire param types to <see cref="ControlParamType"/>. An
    /// unrecognized type throws rather than silently registering a broken action - a manifest this
    /// shell cannot fully understand must not be half-registered.</summary>
    public static IReadOnlyList<ControlAction> ToControlActions(IReadOnlyList<ShowEngineActionDefinition> defs)
    {
        var actions = new List<ControlAction>(defs.Count);
        foreach (var def in defs)
        {
            var pars = new List<ControlParam>(def.Params.Count);
            foreach (var p in def.Params)
            {
                var type = p.Type switch
                {
                    "string" => ControlParamType.String,
                    "int" => ControlParamType.Int,
                    "double" => ControlParamType.Double,
                    "bool" => ControlParamType.Bool,
                    _ => throw new InvalidOperationException($"unknown param type '{p.Type}' on {def.Id}"),
                };
                pars.Add(new ControlParam(p.Name, type, p.Required, p.Description));
            }

            actions.Add(new ControlAction(def.Id, def.Title, def.Description, pars));
        }

        return actions;
    }

    // ------------------------------------------------------------------ supervisor event handlers

    private void OnHandshaken(ShowEngineHandshake handshake)
    {
        IReadOnlyList<ControlAction> mapped;
        try
        {
            mapped = ToControlActions(handshake.Actions);
        }
        catch (InvalidOperationException ex)
        {
            lock (_gate)
            {
                _actions = Array.Empty<ControlAction>();
                _feedbackFieldTemplates = Array.Empty<string>();
            }

            RaiseLog("error", ex.Message);
            RaiseActionsChanged();
            return;
        }

        IReadOnlyList<ShowEngineActionDefinition>? previous;
        int lastCapacity;
        bool capacityEver;
        IReadOnlyList<ShowEngineParticipant> lastRoster;
        bool rosterEver;
        string? lastSpeaker;

        lock (_gate)
        {
            previous = _lastActionDefs;
            _lastActionDefs = handshake.Actions;
            _actions = mapped;
            _feedbackFieldTemplates = handshake.FieldTemplates;

            lastCapacity = _lastCapacity;
            capacityEver = _capacityEverPublished;
            lastRoster = _lastRoster;
            rosterEver = _rosterEverPublished;
            lastSpeaker = _lastActiveSpeaker;
        }

        if (previous is not null)
        {
            var drift = FindManifestDrift(previous, handshake.Actions);
            if (drift is not null)
            {
                RaiseLog("warn",
                    $"engine manifest changed between generations: {previous.Count}→{handshake.Actions.Count} actions, first difference at '{drift}'");
            }
        }

        RaiseActionsChanged();

        // Re-arm order matters (spec): capacity, then roster, then active speaker.
        if (capacityEver) SendCapacity(lastCapacity);
        if (rosterEver) SendRoster(lastRoster);
        if (lastSpeaker is not null) SendActiveSpeaker(lastSpeaker);
    }

    private void OnHealthChanged(ShowEngineHealth health)
    {
        if (health.State is ShowEngineState.Stopped or ShowEngineState.Failed)
        {
            lock (_gate)
            {
                _actions = Array.Empty<ControlAction>();
                _feedbackFieldTemplates = Array.Empty<string>();
            }

            RaiseActionsChanged();
        }

        HealthChanged?.Invoke(this, health);
    }

    private void OnSnapshotReceived(ShowEngineSnapshot snapshot)
    {
        lock (_gate) _latest = snapshot;
        SnapshotChanged?.Invoke(this, snapshot);
    }

    private void OnHostCommandReceived(ShowEngineHostCommand command) => HostCommand?.Invoke(this, command);

    private void OnLogReceived(ShowEngineLogLine log) => Log?.Invoke(this, log);

    // ------------------------------------------------------------------ wire sends

    private void SendCapacity(int capacity) =>
        SendFireAndForget("capacity", new { capacity }, "capacity");

    private void SendRoster(IReadOnlyList<ShowEngineParticipant> roster)
    {
        var participants = roster.Select(p => new
        {
            participantId = p.ParticipantId,
            rawName = p.RawName,
            online = p.Online,
            videoOn = p.VideoOn,
            audioOn = p.AudioOn,
            handRaised = p.HandRaised,
            zoomRole = p.ZoomRole,
        }).ToList();

        SendFireAndForget("zoomEvent", new { @event = new { kind = "roster", participants } }, "roster");
    }

    private void SendActiveSpeaker(string participantId) =>
        SendFireAndForget("activeSpeaker", new { participantId }, "active speaker");

    /// <summary>Issue a request without awaiting it. <see cref="ShowEngineSupervisor.SendAsync"/>
    /// throws SYNCHRONOUSLY (not via a faulted task) when the engine is not Running, so that path is
    /// caught here too - a re-arm racing a mid-teardown health flip must log, not throw out of a
    /// supervisor event handler.</summary>
    private void SendFireAndForget(string type, object? payload, string description)
    {
        Task<System.Text.Json.JsonDocument> send;
        try
        {
            send = _supervisor.SendAsync(type, payload, CancellationToken.None);
        }
        catch (Exception ex)
        {
            RaiseLog("warn", $"failed to send {description} to the show engine: {ex.Message}");
            return;
        }

        _ = send.ContinueWith(t =>
        {
            if (t.IsFaulted)
            {
                var ex = t.Exception?.GetBaseException();
                RaiseLog("warn", $"failed to send {description} to the show engine: {ex?.Message}");
            }
            else if (t.IsCompletedSuccessfully)
            {
                t.Result.Dispose();
            }
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
    }

    /// <summary>First point of drift between two manifests, comparing ids and param TYPES in order
    /// (name/required/description changes are not drift - only shape a caller could break on is).
    /// Returns the drifted action's id from the NEW manifest (it is the one that wins), or the id at
    /// the point one manifest simply ran out if the two differ only in length. Null when identical.</summary>
    private static string? FindManifestDrift(
        IReadOnlyList<ShowEngineActionDefinition> oldDefs, IReadOnlyList<ShowEngineActionDefinition> newDefs)
    {
        var count = Math.Min(oldDefs.Count, newDefs.Count);
        for (var i = 0; i < count; i++)
        {
            if (oldDefs[i].Id != newDefs[i].Id || !ParamTypesEqual(oldDefs[i].Params, newDefs[i].Params))
            {
                return newDefs[i].Id;
            }
        }

        if (oldDefs.Count == newDefs.Count) return null;
        return count < newDefs.Count ? newDefs[count].Id : oldDefs[count].Id;
    }

    private static bool ParamTypesEqual(
        IReadOnlyList<ShowEngineActionParam> a, IReadOnlyList<ShowEngineActionParam> b)
    {
        if (a.Count != b.Count) return false;
        for (var i = 0; i < a.Count; i++)
        {
            if (a[i].Type != b[i].Type) return false;
        }

        return true;
    }

    private void RaiseActionsChanged() => ActionsChanged?.Invoke(this, EventArgs.Empty);

    private void RaiseLog(string level, string message) => Log?.Invoke(this, new ShowEngineLogLine(level, message));

    public void Dispose()
    {
        _supervisor.HealthChanged -= OnHealthChanged;
        _supervisor.Handshaken -= OnHandshaken;
        _supervisor.SnapshotReceived -= OnSnapshotReceived;
        _supervisor.HostCommandReceived -= OnHostCommandReceived;
        _supervisor.LogReceived -= OnLogReceived;
    }
}
