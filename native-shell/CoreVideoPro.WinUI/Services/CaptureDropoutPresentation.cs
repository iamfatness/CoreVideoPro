using System.Globalization;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

internal static class CaptureDropoutPresentation
{
    internal const string UnavailableNotice =
        "This source does not report signal loss yet, so it always shows its last picture.";

    internal static IReadOnlyList<RouteSelectOption> Policies { get; } =
    [
        new() { Value = "hold", Label = "Hold last frame (up to 5 s, then slate)" },
        new() { Value = "black", Label = "Black (then slate)" }
    ];

    internal static string Status(NativeMediaCoreSourceHealth? source)
    {
        if (source is not { Kind: "capture", DropoutPolicyApplies: true })
        {
            return "";
        }

        var reason = string.IsNullOrWhiteSpace(source.LossReason) ? "signal loss" : source.LossReason.Replace('-', ' ');
        return (source.Liveness, source.OnAir) switch
        {
            ("lost", "failed") => $"Lost ({reason}) — slate on Program",
            ("lost", _) => $"Lost ({reason}) — " +
                (source.DropoutPolicy == "black" ? "Program black" : "holding") +
                $", slate in {Seconds(source.FailInMs)}",
            ("stalled", _) => $"No new frames for {Seconds(source.LastSampleAgeMs)} — " +
                (source.DropoutPolicy == "black" ? "Program black" : "holding last frame"),
            ("live", _) => "Live",
            _ => ""
        };
    }

    private static string Seconds(double? milliseconds) => milliseconds is double value
        ? (value / 1000).ToString("0.0", CultureInfo.InvariantCulture) + " s"
        : "unknown";
}
