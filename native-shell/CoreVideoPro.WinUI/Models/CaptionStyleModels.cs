namespace CoreVideoPro.WinUI.Models;

public sealed class CaptionStyleState
{
    public required string FontSize { get; init; }
    public required string TextColor { get; init; }
    public required int BackgroundOpacity { get; init; }
    public required bool Uppercase { get; init; }
}