namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// Normalized 0-1 rectangle within the 16:9 program frame.
/// </summary>
public sealed class NormalizedCanvasRect
{
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; } = 1;
    public double Height { get; set; } = 1;

    public NormalizedCanvasRect Clone() =>
        new()
        {
            X = X,
            Y = Y,
            Width = Width,
            Height = Height
        };

    public void Clamp()
    {
        Width = Math.Clamp(Width, 0.08, 1);
        Height = Math.Clamp(Height, 0.08, 1);
        X = Math.Clamp(X, 0, 1 - Width);
        Y = Math.Clamp(Y, 0, 1 - Height);
    }
}

public enum SceneCanvasPreset
{
    Full,
    TwoUp,
    Pip,
    Grid,
    SpeakerSlides
}
