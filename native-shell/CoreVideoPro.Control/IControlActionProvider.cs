namespace CoreVideoPro.Control;

/// <summary>Where an action (or, via a provider's <see cref="IControlActionProvider.DefaultOscExposure"/>,
/// a whole provider's action set) is reachable from over OSC.</summary>
public enum OscExposure
{
    /// <summary>Reachable from any LAN client.</summary>
    Lan,

    /// <summary>Reachable only from the local loopback interface.</summary>
    LoopbackOnly
}

/// <summary>A source of remotely-invocable actions that composes into a <see cref="ControlCatalog"/>
/// alongside the static <see cref="ControlActionRegistry"/> table. Implementations own a live,
/// possibly-changing set of actions (e.g. actions generated per show-engine session) and raise
/// <see cref="ActionsChanged"/> whenever that set changes so the catalog can re-merge.</summary>
public interface IControlActionProvider
{
    /// <summary>A stable identifier for this provider (used only for diagnostics/logging).</summary>
    string Id { get; }

    /// <summary>The OSC exposure applied to every action this provider currently contributes,
    /// unless overridden elsewhere.</summary>
    OscExposure DefaultOscExposure { get; }

    /// <summary>The provider's current action set. Read live — the catalog re-reads this
    /// whenever <see cref="ActionsChanged"/> fires.</summary>
    IReadOnlyList<ControlAction> Actions { get; }

    /// <summary>Feedback-field templates (see <see cref="ControlManifest.StateFields"/>) this
    /// provider contributes to the merged feedback field list.</summary>
    IReadOnlyList<string> FeedbackFieldTemplates { get; }

    /// <summary>Raised whenever <see cref="Actions"/> changes. Never throws from a subscriber's
    /// perspective — the catalog handles all validation/merge errors internally.</summary>
    event EventHandler? ActionsChanged;
}
