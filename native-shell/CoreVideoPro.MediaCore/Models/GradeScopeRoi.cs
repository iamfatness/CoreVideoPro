namespace CoreVideoPro.MediaCore.Models;

/// <summary>Editor measurement preference in source-image coordinates; never part of a grade.</summary>
public sealed record GradeScopeRoi(bool Enabled = false, double X = 0, double Y = 0,
    double Width = 1, double Height = 1, long Revision = 0, string Shape = "rectangle")
{
    private bool HasValidFields => double.IsFinite(X) && double.IsFinite(Y) && double.IsFinite(Width) && double.IsFinite(Height) &&
        X >= 0 && Y >= 0 && Width > 0 && Height > 0 && Revision >= 0 && Shape is "rectangle" or "circle";
    public bool IsValid => HasValidFields && X+Width<=1.000000001 && Y+Height<=1.000000001;
    internal bool IsValidMeasurement => HasValidFields && X+Width<=1.0000005 && Y+Height<=1.0000005;
    [System.Text.Json.Serialization.JsonIgnore]
    public int ExpectedSampleCount => Enabled && Shape == "circle" ? CircleSampleCount : 256 * 144;
    // Native JSON reports six significant decimal digits. Revision remains exact;
    // tolerate only its coordinate formatting error (under .005 pixel at 8K).
    public bool MatchesMeasurement(GradeScopeRoi? other) => IsValid && other is {IsValidMeasurement:true} &&
        Enabled==other.Enabled && Shape==other.Shape && Revision==other.Revision &&
        Math.Abs(X-other.X)<=.0000005 && Math.Abs(Y-other.Y)<=.0000005 &&
        Math.Abs(Width-other.Width)<=.0000005 && Math.Abs(Height-other.Height)<=.0000005;
    private static readonly int CircleSampleCount = CountCircleSamples();
    private static int CountCircleSamples()
    {
        var count = 0;
        for (var y = 0; y < 144; ++y) for (var x = 0; x < 256; ++x)
        {
            var nx = (x + .5) / 256 * 2 - 1; var ny = (y + .5) / 144 * 2 - 1;
            if (nx * nx + ny * ny <= 1) ++count;
        }
        return count;
    }
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
