using System.Text.RegularExpressions;

namespace CoreVideoPro.Control;

/// <summary>Composes the static <see cref="ControlActionRegistry"/> table with zero or more
/// live <see cref="IControlActionProvider"/>s into one merged action catalog. This is the seam
/// that lets a host (e.g. the OHG show engine) contribute its own remotely-invocable actions
/// without touching the hand-authored static table.
///
/// The static registry's actions always come first, in their declared order; each included
/// provider's actions follow, in provider order. A provider whose current action set contains a
/// duplicate id (against the static table, another provider, or itself) or an id that fails
/// <see cref="ActionIdPattern"/> is EXCLUDED WHOLESALE — none of its actions appear — and
/// <see cref="LastValidationError"/> names the offending id. Validation re-runs, and the merged
/// snapshot rebuilds, on construction and every time any provider raises
/// <see cref="IControlActionProvider.ActionsChanged"/>; the rebuild never throws from the event
/// handler.
///
/// Thread-safety: providers may raise <c>ActionsChanged</c> from a background thread. A private
/// lock serializes concurrent rebuilds; each rebuild constructs a brand-new immutable snapshot
/// (list + dictionaries + the validation error, all in ONE <c>Snapshot</c> record so they can
/// never disagree) and publishes it by assigning the <c>volatile</c> <c>_snapshot</c> field.
/// Every reader (<see cref="Actions"/>, <see cref="TryGet"/>, <see cref="Contains"/>,
/// <see cref="ExposureOf"/>, <see cref="FeedbackFields"/>, <see cref="TryBind"/>,
/// <see cref="LastValidationError"/>) reads that same field with no lock of its own — the
/// volatile write/read pair gives the reader an acquire fence, so it can never observe a
/// half-built or torn snapshot, or a snapshot's actions disagreeing with its own validation
/// error.</summary>
public sealed class ControlCatalog
{
    public static readonly Regex ActionIdPattern = new("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9]*)+$");

    /// <summary>A shared singleton catalog wrapping the static registry alone, with no providers.</summary>
    public static readonly ControlCatalog StaticOnly = new(Array.Empty<IControlActionProvider>());

    private readonly IReadOnlyList<IControlActionProvider> _providers;
    private readonly object _lock = new();

    // Providers (Task 6/8+) raise ActionsChanged from a background reader thread. The rebuild
    // itself is serialized under _lock, but readers (Actions/TryGet/Contains/ExposureOf/
    // FeedbackFields/TryBind/LastValidationError) take no lock at all — they just read this
    // field. `volatile` gives those reads an acquire fence pairing with the release-semantics
    // write in Rebuild()'s caller, so a reader can never observe a torn/reordered view of the
    // fields inside a freshly published Snapshot. ValidationError lives INSIDE Snapshot (not a
    // sibling field) so the two can never be published out of sync with each other.
    private volatile Snapshot _snapshot;

    public ControlCatalog(IEnumerable<IControlActionProvider> providers)
    {
        _providers = providers.ToList();
        foreach (var provider in _providers)
        {
            provider.ActionsChanged += OnProviderActionsChanged;
        }

        _snapshot = Rebuild();
    }

    public event EventHandler? Changed;

    public string? LastValidationError => _snapshot.ValidationError;

    public IReadOnlyList<ControlAction> Actions => _snapshot.Actions;

    public IReadOnlyList<string> FeedbackFields => _snapshot.FeedbackFields;

    public bool TryGet(string actionId, out ControlAction action) => _snapshot.ById.TryGetValue(actionId, out action!);

    public bool Contains(string actionId) => _snapshot.ById.ContainsKey(actionId);

    public OscExposure ExposureOf(string actionId) =>
        _snapshot.ExposureById.TryGetValue(actionId, out var exposure) ? exposure : OscExposure.Lan;

    /// <summary>Validates and coerces raw positional args against the action's param schema. See
    /// <see cref="ControlActionRegistry.TryBind"/> for the exact semantics — identical here,
    /// just resolved against the merged catalog instead of the static table alone.</summary>
    public bool TryBind(
        string actionId,
        IReadOnlyList<object?> rawArgs,
        out IReadOnlyList<object?> bound,
        out string? error)
    {
        bound = Array.Empty<object?>();
        if (!TryGet(actionId, out var action))
        {
            error = $"Unknown action '{actionId}'.";
            return false;
        }

        var result = new object?[action.Params.Count];
        for (var i = 0; i < action.Params.Count; i++)
        {
            var param = action.Params[i];
            var raw = i < rawArgs.Count ? rawArgs[i] : null;
            if (raw is null)
            {
                if (param.Required)
                {
                    error = $"Action '{actionId}' requires parameter '{param.Name}' at position {i}.";
                    return false;
                }

                result[i] = null;
                continue;
            }

            if (!TryCoerce(raw, param.Type, out var coerced))
            {
                error = $"Action '{actionId}' parameter '{param.Name}' must be {param.Type} (got '{raw}').";
                return false;
            }

            result[i] = coerced;
        }

        bound = result;
        error = null;
        return true;
    }

    private void OnProviderActionsChanged(object? sender, EventArgs e)
    {
        try
        {
            lock (_lock)
            {
                _snapshot = Rebuild();
            }
        }
        catch
        {
            // Never throw from an event handler; a failed rebuild leaves the previous snapshot
            // (and LastValidationError) in place.
            return;
        }

        Changed?.Invoke(this, EventArgs.Empty);
    }

    private Snapshot Rebuild()
    {
        var actions = new List<ControlAction>(ControlActionRegistry.Actions);
        var exposureById = new Dictionary<string, OscExposure>(StringComparer.Ordinal);
        var seenIds = new HashSet<string>(actions.Select(a => a.Id), StringComparer.Ordinal);
        var feedbackFields = new List<string>(ControlManifest.StateFields);
        string? validationError = null;

        foreach (var provider in _providers)
        {
            var providerActions = provider.Actions;
            var providerIds = new HashSet<string>(StringComparer.Ordinal);
            string? providerError = null;

            foreach (var action in providerActions)
            {
                if (!ActionIdPattern.IsMatch(action.Id))
                {
                    providerError = $"Provider '{provider.Id}' action id '{action.Id}' is invalid (does not match {ActionIdPattern}).";
                    break;
                }

                if (seenIds.Contains(action.Id) || !providerIds.Add(action.Id))
                {
                    providerError = $"Provider '{provider.Id}' action id '{action.Id}' is a duplicate.";
                    break;
                }
            }

            if (providerError is not null)
            {
                validationError ??= providerError;
                continue;
            }

            foreach (var action in providerActions)
            {
                actions.Add(action);
                seenIds.Add(action.Id);
                exposureById[action.Id] = provider.DefaultOscExposure;
            }

            feedbackFields.AddRange(provider.FeedbackFieldTemplates);
        }

        var byId = actions.ToDictionary(a => a.Id, StringComparer.Ordinal);
        return new Snapshot(actions, byId, exposureById, feedbackFields, validationError);
    }

    internal static bool TryCoerce(object raw, ControlParamType type, out object? value)
    {
        value = null;
        try
        {
            switch (type)
            {
                case ControlParamType.String:
                    value = raw as string ?? Convert.ToString(raw, System.Globalization.CultureInfo.InvariantCulture);
                    return value is not null;
                case ControlParamType.Int:
                    value = raw switch
                    {
                        int i => i,
                        long l => (int)l,
                        double d => (int)Math.Round(d),
                        float f => (int)MathF.Round(f),
                        bool b => b ? 1 : 0,
                        string s when int.TryParse(s, System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var parsed) => parsed,
                        _ => (object?)null
                    };
                    return value is not null;
                case ControlParamType.Double:
                    value = raw switch
                    {
                        double d => d,
                        float f => (double)f,
                        int i => (double)i,
                        long l => (double)l,
                        string s when double.TryParse(s, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsed) => parsed,
                        _ => (object?)null
                    };
                    return value is not null;
                case ControlParamType.Bool:
                    value = raw switch
                    {
                        bool b => b,
                        int i => i != 0,
                        long l => l != 0,
                        double d => Math.Abs(d) > double.Epsilon,
                        float f => MathF.Abs(f) > float.Epsilon,
                        string s when bool.TryParse(s, out var parsed) => parsed,
                        string s when s is "1" => true,
                        string s when s is "0" => false,
                        _ => (object?)null
                    };
                    return value is not null;
                default:
                    return false;
            }
        }
        catch
        {
            return false;
        }
    }

    private sealed record Snapshot(
        IReadOnlyList<ControlAction> Actions,
        IReadOnlyDictionary<string, ControlAction> ById,
        IReadOnlyDictionary<string, OscExposure> ExposureById,
        IReadOnlyList<string> FeedbackFields,
        string? ValidationError);
}
