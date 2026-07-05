namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Pure curve math for the processing workspace (C6): the drawn responses are
/// computed from the SAME filter formulas the core runs (RBJ audio-EQ-cookbook
/// biquads mirroring native AudioDsp.h; compressor/gate static transfer
/// functions with the chain's parameters), so the graphs show the actual
/// processing, not an illustration. All functions return normalized [0..1]
/// point lists ready to scale into pixels.
/// </summary>
public static class DspCurveMath
{
    public const double MinFrequencyHz = 20;
    public const double MaxFrequencyHz = 20000;
    public const double MinDb = -24;
    public const double MaxDb = 12;

    private readonly record struct Biquad(double B0, double B1, double B2, double A1, double A2);

    private static Biquad Highpass(double sampleRate, double frequencyHz, double q = 0.7071)
    {
        var w0 = 2 * Math.PI * frequencyHz / sampleRate;
        var cosW0 = Math.Cos(w0);
        var alpha = Math.Sin(w0) / (2 * q);
        var a0 = 1 + alpha;
        return new Biquad(
            (1 + cosW0) / 2 / a0,
            -(1 + cosW0) / a0,
            (1 + cosW0) / 2 / a0,
            -2 * cosW0 / a0,
            (1 - alpha) / a0);
    }

    private static Biquad Peaking(double sampleRate, double frequencyHz, double gainDb, double q = 1.0)
    {
        var amplitude = Math.Pow(10, gainDb / 40);
        var w0 = 2 * Math.PI * frequencyHz / sampleRate;
        var cosW0 = Math.Cos(w0);
        var alpha = Math.Sin(w0) / (2 * q);
        var a0 = 1 + alpha / amplitude;
        return new Biquad(
            (1 + alpha * amplitude) / a0,
            -2 * cosW0 / a0,
            (1 - alpha * amplitude) / a0,
            -2 * cosW0 / a0,
            (1 - alpha / amplitude) / a0);
    }

    /// <summary>|H(e^jw)| of a biquad at frequency f — the standard z-plane magnitude.</summary>
    private static double MagnitudeAt(in Biquad biquad, double frequencyHz, double sampleRate)
    {
        var w = 2 * Math.PI * frequencyHz / sampleRate;
        var cos1 = Math.Cos(w);
        var sin1 = Math.Sin(w);
        var cos2 = Math.Cos(2 * w);
        var sin2 = Math.Sin(2 * w);
        var numReal = biquad.B0 + biquad.B1 * cos1 + biquad.B2 * cos2;
        var numImag = -(biquad.B1 * sin1 + biquad.B2 * sin2);
        var denReal = 1 + biquad.A1 * cos1 + biquad.A2 * cos2;
        var denImag = -(biquad.A1 * sin1 + biquad.A2 * sin2);
        var num = Math.Sqrt(numReal * numReal + numImag * numImag);
        var den = Math.Sqrt(denReal * denReal + denImag * denImag);
        return den <= 1e-12 ? 0 : num / den;
    }

    /// <summary>The 8-band centers — MUST match kEqBandHz in native AudioDsp.h.</summary>
    public static readonly double[] EqBandHz = [63, 125, 250, 500, 1000, 2000, 4000, 8000];

    /// <summary>
    /// 8-band EQ frequency response (C6b: high-pass + up to eight peaking bands
    /// at the octave centers, mirroring the native chain — flat bands skipped)
    /// as normalized points: x = log-frequency position [0..1], y = gain mapped
    /// [0..1] where 0 = MinDb and 1 = MaxDb.
    /// </summary>
    public static IReadOnlyList<(double X, double Y)> EqResponse(
        double highpassHz, IReadOnlyList<double> bandGainsDb,
        int pointCount = 64, double sampleRate = 48000)
    {
        var cascade = new List<Biquad> { Highpass(sampleRate, Math.Clamp(highpassHz, 20, 400)) };
        for (var band = 0; band < EqBandHz.Length; band++)
        {
            var gainDb = band < bandGainsDb.Count ? Math.Clamp(bandGainsDb[band], -12, 12) : 0;
            if (Math.Abs(gainDb) >= 0.05)
            {
                cascade.Add(Peaking(sampleRate, EqBandHz[band], gainDb, 1.4));
            }
        }

        var points = new List<(double, double)>(pointCount);
        var logMin = Math.Log10(MinFrequencyHz);
        var logMax = Math.Log10(MaxFrequencyHz);
        for (var index = 0; index < pointCount; index++)
        {
            var x = index / (double)(pointCount - 1);
            var frequency = Math.Pow(10, logMin + x * (logMax - logMin));
            var magnitude = 1.0;
            foreach (var stage in cascade)
            {
                magnitude *= MagnitudeAt(stage, frequency, sampleRate);
            }

            var db = 20 * Math.Log10(Math.Max(magnitude, 1e-6));
            var y = (Math.Clamp(db, MinDb, MaxDb) - MinDb) / (MaxDb - MinDb);
            points.Add((x, y));
        }

        return points;
    }

    /// <summary>
    /// Compressor static transfer curve (input dB → output dB over [-60..0]),
    /// normalized to [0..1] both axes. C7c: knee-aware — quadratic interpolation
    /// across the knee width, matching applyStereoCompressor's gain computer.
    /// </summary>
    public static IReadOnlyList<(double X, double Y)> CompressorTransfer(
        double thresholdDb, double ratio, double kneeDb = 0, int pointCount = 48)
    {
        var points = new List<(double, double)>(pointCount);
        var clampedRatio = Math.Max(1, ratio);
        var slope = 1.0 - 1.0 / clampedRatio;
        var knee = Math.Max(0, kneeDb);
        for (var index = 0; index < pointCount; index++)
        {
            var x = index / (double)(pointCount - 1);
            var inputDb = -60 + x * 60;
            var over = inputDb - thresholdDb;
            double grDb = 0;
            if (knee > 0 && over > -knee / 2 && over < knee / 2)
            {
                var k = over + knee / 2;
                grDb = slope * k * k / (2 * knee);
            }
            else if (over >= knee / 2)
            {
                grDb = slope * over;
            }

            points.Add((x, (Math.Clamp(inputDb - grDb, -60, 0) + 60) / 60));
        }

        return points;
    }

    /// <summary>
    /// Gate static transfer curve: below the threshold the output drops by the
    /// RANGE (C7c - matches the core floor-not-silence gate); above it, unity.
    /// </summary>
    public static IReadOnlyList<(double X, double Y)> GateTransfer(
        double thresholdDb, double rangeDb = -80, int pointCount = 48)
    {
        var points = new List<(double, double)>(pointCount);
        for (var index = 0; index < pointCount; index++)
        {
            var x = index / (double)(pointCount - 1);
            var inputDb = -80 + x * 80;
            var outputDb = inputDb < thresholdDb ? Math.Max(-80, inputDb + rangeDb) : inputDb;
            points.Add((x, (Math.Clamp(outputDb, -80, 0) + 80) / 80));
        }

        return points;
    }
}
