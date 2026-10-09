namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    // Read the saved policy even for an assigned guest currently out of the room.
    public string SourceDropoutPolicy(string sourceId) =>
        _sourceDropoutPolicies.TryGetValue(sourceId, out var policy) ? policy : "hold";
}
