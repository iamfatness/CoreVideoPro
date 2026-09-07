namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>The result of one Take invocation, independent of later UI status changes.</summary>
public sealed record TakeResult(bool Succeeded, string? Error = null)
{
    public static TakeResult Success { get; } = new(true);
    public static TakeResult Failed(string error) => new(false, error);
}
