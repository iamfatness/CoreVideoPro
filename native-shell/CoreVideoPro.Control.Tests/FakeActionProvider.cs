using CoreVideoPro.Control;

namespace CoreVideoPro.Control.Tests;

/// <summary>A minimal <see cref="IControlActionProvider"/> for tests: constructed with an id,
/// a default OSC exposure, and an initial action set; <see cref="Set"/> replaces the action set
/// and raises <see cref="ActionsChanged"/>.</summary>
public sealed class FakeActionProvider : IControlActionProvider
{
    public FakeActionProvider(string id, OscExposure exposure, params ControlAction[] actions)
    {
        Id = id;
        DefaultOscExposure = exposure;
        Actions = actions;
    }

    public string Id { get; }

    public OscExposure DefaultOscExposure { get; }

    public IReadOnlyList<ControlAction> Actions { get; private set; }

    public IReadOnlyList<string> FeedbackFieldTemplates { get; set; } = Array.Empty<string>();

    public event EventHandler? ActionsChanged;

    public void Set(params ControlAction[] actions)
    {
        Actions = actions;
        ActionsChanged?.Invoke(this, EventArgs.Empty);
    }
}
