namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// #480: ComboBox ItemsSource rebuilds fire SelectionChanged with the blank
/// placeholder. Only an operator gesture may change a layer's source.
/// </summary>
public static class LayerSourceSelectionPolicy
{
    public const string OperatorCause = "operator";
    public const string RefreshIgnoredCause = "refresh-ignored";

    public static bool ShouldCommit(bool operatorGesture, string? incomingValue)
        => operatorGesture && incomingValue is not null;

    public static string FormatLog(int layerIndex, string? source, string cause)
        => $"scene source selected: layer={layerIndex} source={source} by={cause}";
}
