using System.Globalization;

namespace CoreVideoPro.MediaCore.Models;

public sealed record CubeLut(int Size, double[] DomainMin, double[] DomainMax, double[][] Samples);
public static class CubeLutParser
{
    public static CubeLut Parse(string text)
    {
        if (text is null || System.Text.Encoding.UTF8.GetByteCount(text) > 2_000_000) throw new ArgumentException("LUT files are limited to 2 MB.");
        var size = 0; var min = new double[3]; var max = new[] { 1d,1d,1d }; var samples = new List<double[]>();
        bool minSeen = false, maxSeen = false;
        foreach (var raw in text.Split('\n')) {
            var line = raw.Split('#',2)[0].Trim(); if (line.Length == 0) continue;
            var tokens = line.Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries);
            if (tokens[0] == "TITLE") continue;
            if (tokens[0] == "LUT_3D_SIZE") {
                if (size != 0 || tokens.Length != 2 || !int.TryParse(tokens[1],out size) || size is < 2 or > 33)
                    throw new ArgumentException("Supported 3D LUT dimensions are 2 through 33; duplicate size declarations are invalid.");
                continue;
            }
            if (tokens[0] is "DOMAIN_MIN" or "DOMAIN_MAX") {
                var minimum = tokens[0] == "DOMAIN_MIN";
                if ((minimum && minSeen) || (!minimum && maxSeen) || tokens.Length != 4) throw new ArgumentException("Invalid LUT domain.");
                var values = tokens.Skip(1).Select(Number).ToArray();
                if (minimum) { min = values; minSeen = true; } else { max = values; maxSeen = true; }
                continue;
            }
            if (size == 0 || tokens.Length != 3 || samples.Count >= size*size*size) throw new ArgumentException("Unsupported or malformed .cube content.");
            var sample = tokens.Select(Number).ToArray();
            if (sample.Any(v=>v is < -16 or > 16)) throw new ArgumentException("LUT outputs must be within -16 to 16.");
            samples.Add(sample);
        }
        if (size == 0 || samples.Count != size*size*size || Enumerable.Range(0,3).Any(i=>(float)max[i] <= (float)min[i] || Math.Abs(min[i])>16 || Math.Abs(max[i])>16))
            throw new ArgumentException("LUT sample count or domain is invalid.");
        return new(size,min,max,samples.ToArray());
    }
    private static double Number(string value) => double.TryParse(value,NumberStyles.Float,CultureInfo.InvariantCulture,out var n) && double.IsFinite(n)
        ? n : throw new ArgumentException("LUT contains a non-finite or invalid number.");
}
