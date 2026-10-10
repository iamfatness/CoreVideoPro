namespace CoreVideoPro.MediaCore.Models;

/// <summary>Editor measurement preference in source-image coordinates; never part of a grade.</summary>
public sealed record GradeScopeRoi(bool Enabled = false, double X = 0, double Y = 0,
    double Width = 1, double Height = 1, long Revision = 0)
{
    public bool IsValid => double.IsFinite(X) && double.IsFinite(Y) && double.IsFinite(Width) && double.IsFinite(Height) &&
        X >= 0 && Y >= 0 && Width > 0 && Height > 0 && X + Width <= 1.000000001 && Y + Height <= 1.000000001 && Revision >= 0;
    public (int X, int Y, int Width, int Height) Pixels(int width, int height)
    {
        if (!IsValid || width <= 0 || height <= 0) throw new ArgumentException("Invalid source region.");
        if (!Enabled) return (0, 0, width, height);
        var left = Math.Clamp((int)Math.Floor(X * width), 0, width - 1);
        var top = Math.Clamp((int)Math.Floor(Y * height), 0, height - 1);
        var right = Math.Clamp((int)Math.Ceiling((X + Width) * width), left + 1, width);
        var bottom = Math.Clamp((int)Math.Ceiling((Y + Height) * height), top + 1, height);
        return (left, top, right - left, bottom - top);
    }
}
