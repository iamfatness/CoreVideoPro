using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

public enum PlacementSeverity
{
    None,
    Informational,
    Warning,
}

public sealed record PlacementReadout(PlacementSeverity Severity, IReadOnlyList<string> Lines)
{
    public static PlacementReadout Empty { get; } = new(PlacementSeverity.None, []);
}

/// <summary>
/// #935: names each ISO (and Program) that records on the CPU software encoder and shows its
/// MEASURED per-frame cost. Pure and WinUI-free. Owner ruling D9: a software writer is
/// Informational; Warning only when it dropped frames or its measured mean work per frame exceeds
/// its measured frame budget. Nothing is estimated: cost needs a positive work total and frame
/// count, the budget needs a reported frame rate, and anything absent reads "cost not measured
/// yet" or is left out (docs/reference/honest-reporting.md).
/// </summary>
public static class IsoEncoderPlacementReadout
{
    public static PlacementReadout Describe(NativeMediaCoreRecordingSession? recording, bool programHardwareAccelerated)
    {
        // The core decides placement at arm; before a take there is nothing measured to show.
        if (recording is not { Active: true })
        {
            return PlacementReadout.Empty;
        }

        var lines = new List<string>();
        var warn = false;

        var program = recording.Streams.FirstOrDefault(static s =>
            string.Equals(s.Kind, "program", StringComparison.OrdinalIgnoreCase));
        if (program is not null)
        {
            // #935 B1: only the MEASURED path (read back from the encoder the Program writer
            // instantiated) may claim software. Absent = not measured yet = no claim either way;
            // programHardwareAccelerated is never used to infer placement.
            var onSoftware = string.Equals(program.EncoderPath, "software", StringComparison.OrdinalIgnoreCase);
            if (onSoftware)
            {
                lines.Add(BuildLine("Program is on the CPU software encoder", program, out var w));
                warn |= w;
            }
        }

        var software = recording.Streams
            .Where(static s => string.Equals(s.Kind, "iso", StringComparison.OrdinalIgnoreCase) &&
                               string.Equals(s.EncoderPath, "software", StringComparison.OrdinalIgnoreCase))
            .Select((s, i) => (s, i))
            .OrderBy(static t => t.s.IsoIndex ?? int.MaxValue)
            .ThenBy(static t => t.i);
        foreach (var (stream, _) in software)
        {
            var name = string.IsNullOrWhiteSpace(stream.DisplayName)
                ? stream.SourceId ?? stream.ParticipantId ?? "ISO"
                : stream.DisplayName;
            var label = stream.IsoIndex is { } index ? $"ISO-{index} {name}" : name;
            var reason = DescribeReason(stream.FallbackReason);
            var head = $"{label} is on the CPU software encoder" + (reason is null ? string.Empty : $" ({reason})");
            lines.Add(BuildLine(head, stream, out var w));
            warn |= w;
        }

        if (lines.Count == 0)
        {
            return PlacementReadout.Empty;
        }

        return new PlacementReadout(warn ? PlacementSeverity.Warning : PlacementSeverity.Informational, lines);
    }

    private static string BuildLine(string head, NativeMediaCoreRecordingStream stream, out bool warning)
    {
        warning = stream.DroppedFrames > 0;
        var parts = new List<string> { head };

        if (stream.VideoWorkUs is > 0 && stream.MuxVideoFrameCount is > 0)
        {
            var costMs = stream.VideoWorkUs.Value / (double)stream.MuxVideoFrameCount.Value / 1000.0;
            var cost = costMs.ToString("0.0", CultureInfo.InvariantCulture) + " ms per frame";
            if (stream.FrameRate is > 0)
            {
                var budgetMs = 1000.0 / stream.FrameRate.Value;
                cost += " of a " + budgetMs.ToString("0.0", CultureInfo.InvariantCulture) + " ms budget";
                warning |= costMs > budgetMs;
            }

            parts.Add(cost);
        }
        else
        {
            parts.Add("cost not measured yet");
        }

        parts.Add(stream.DroppedFrames == 1 ? "1 frame dropped" : $"{stream.DroppedFrames} frames dropped");
        return string.Join(" · ", parts);
    }

    private static string? DescribeReason(string? reason) => reason switch
    {
        null or "" => null,
        "hardware-capacity-exhausted" => "GPU sessions used up",
        "hardware-unavailable" => "no GPU encoder",
        "operator-selected" => "set to software",
        "hardware-requested-software-used" => "GPU encoder requested, software encoder used",
        _ => reason,
    };
}
