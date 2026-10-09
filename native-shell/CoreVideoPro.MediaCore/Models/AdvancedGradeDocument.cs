using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>Portable SDR grade content. Arrays are copied at editing boundaries.</summary>
public sealed record GradeCurvePoint([property: JsonPropertyName("x")] double X, [property: JsonPropertyName("y")] double Y);

public sealed record GradeOperation
{
    [JsonIgnore] public string DisplayName => string.IsNullOrWhiteSpace(Name) ? Kind : Name;
    [JsonPropertyName("id")]
    public string Id { get; init; } = Guid.NewGuid().ToString("N");
    [JsonPropertyName("kind")]
    public string Kind { get; init; } = "primaries";
    [JsonPropertyName("enabled")]
    public bool Enabled { get; init; } = true;
    [JsonPropertyName("intensity")]
    public double Intensity { get; init; } = 1;
    [JsonPropertyName("exposureStops")]
    public double ExposureStops { get; init; }
    [JsonPropertyName("contrast")]
    public double Contrast { get; init; } = 1;
    [JsonPropertyName("pivot")]
    public double Pivot { get; init; } = .5;
    [JsonPropertyName("saturation")]
    public double Saturation { get; init; } = 1;
    [JsonPropertyName("temperature")]
    public double Temperature { get; init; }
    [JsonPropertyName("tint")]
    public double Tint { get; init; }
    [JsonPropertyName("lift")]
    public double Lift { get; init; }
    [JsonPropertyName("gamma")]
    public double Gamma { get; init; } = 1;
    [JsonPropertyName("gain")]
    public double Gain { get; init; } = 1;
    [JsonPropertyName("curves")]
    public GradeCurvePoint[][] Curves { get; init; } = IdentityCurves();
    [JsonPropertyName("cubeText")]
    public string CubeText { get; init; } = "";
    [JsonPropertyName("cubeSha256")]
    public string CubeSha256 { get; init; } = "";
    [JsonPropertyName("name")]
    public string Name { get; init; } = "";

    public static GradeCurvePoint[][] IdentityCurves() => Enumerable.Range(0, 4)
        .Select(_ => new[] { new GradeCurvePoint(0, 0), new GradeCurvePoint(1, 1) }).ToArray();

    public GradeOperation Copy() => this with { Curves = Curves.Select(c => c.ToArray()).ToArray() };
}

public sealed record AdvancedGradeDocument
{
    [JsonPropertyName("version")]
    public int Version { get; init; } = 2;
    [JsonPropertyName("colorSpace")]
    public string ColorSpace { get; init; } = "rec709-sdr";
    [JsonPropertyName("bypass")]
    public bool Bypass { get; init; }
    [JsonPropertyName("intensity")]
    public double Intensity { get; init; } = 1;
    [JsonPropertyName("operations")]
    public GradeOperation[] Operations { get; init; } = [];

    public AdvancedGradeDocument Copy() => this with { Operations = Operations.Select(o => o.Copy()).ToArray() };

    public void Validate()
    {
        if (Version != 2 || ColorSpace != "rec709-sdr")
            throw new ArgumentException("Unsupported grade version or color space.");
        Range(Intensity, 0, 1);
        if (Operations is null || Operations.Length > 8 || Operations.Any(o=>o is null) || Operations.Select(o => o.Id).Distinct().Count() != Operations.Length)
            throw new ArgumentException("Grades allow eight uniquely identified adjustments.");
        if (Operations.Sum(o => System.Text.Encoding.UTF8.GetByteCount(o?.CubeText ?? "")) > 1500000) throw new ArgumentException("A grade can embed up to 1.5 MB of LUT text.");
        foreach (var op in Operations)
        {
            if (op is null) throw new ArgumentException("Invalid adjustment.");
            if (string.IsNullOrWhiteSpace(op.Id) || System.Text.Encoding.UTF8.GetByteCount(op.Id) > 64 || op.Kind is not ("primaries" or "curves" or "cube"))
                throw new ArgumentException("Unsupported grade adjustment.");
            Range(op.Intensity, 0, 1); Range(op.ExposureStops, -8, 8);
            Range(op.Contrast, 0, 4); Range(op.Pivot, 0, 1); Range(op.Saturation, 0, 4);
            Range(op.Temperature, -1, 1); Range(op.Tint, -1, 1); Range(op.Lift, -1, 1);
            Range(op.Gamma, .1, 4); Range(op.Gain, 0, 4);
            if (op.Kind == "cube") {
                if (string.IsNullOrEmpty(op.CubeText)) throw new ArgumentException("LUT content is required.");
                CubeLutParser.Parse(op.CubeText);
                var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(op.CubeText))).ToLowerInvariant();
                if (op.CubeSha256 != hash) throw new ArgumentException("LUT content does not match its stored hash.");
            }
            if (op.Curves is null || op.Curves.Length != 4) throw new ArgumentException("Four curve channels are required.");
            foreach (var curve in op.Curves)
            {
                if (curve is null || curve.Length is < 2 or > 16 || curve.Any(p=>p is null) || curve[0].X != 0 || curve[^1].X != 1)
                    throw new ArgumentException("Curves require endpoints and at most sixteen points.");
                float previous = -1;
                foreach (var point in curve)
                {
                    if (point is null) throw new ArgumentException("Invalid curve point.");
                    Range(point.X, 0, 1); Range(point.Y, 0, 1);
                    if ((float)point.X <= previous) throw new ArgumentException("Curve inputs must be strictly ordered.");
                    previous = (float)point.X;
                }
            }
        }
    }

    private static void Range(double value, double min, double max)
    {
        if (!double.IsFinite(value) || value < min || value > max)
            throw new ArgumentException("Grade parameter is outside its supported range.");
    }
}
