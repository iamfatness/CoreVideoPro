using System;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Parse/format for the PERSISTED Zoom→program audio topology (prefs v9,
/// docs/superpowers/specs/2026-08-10-zoom-audio-mode-persistence-design.md).
///
/// Stored as a string rather than a bool because <see cref="ZoomAudioMode"/> is
/// already an enum, a third topology is plausible (mix + selected stems), and a
/// string can express "unrecognized" — which is the safety property here:
/// anything this cannot read becomes <see cref="ZoomAudioMode.PerGuestIso"/>.
/// Per-guest stems are the product default so every ISO source has a real,
/// independently controllable path to Program L/R. ProgramMix remains an
/// explicit compatibility/fallback selection.
/// </summary>
public static class ZoomAudioModePreference
{
    public const string ProgramMixValue = "programMix";
    public const string PerGuestIsoValue = "perGuestIso";

    public static ZoomAudioMode Parse(string? persisted) =>
        string.Equals(persisted?.Trim(), ProgramMixValue, StringComparison.OrdinalIgnoreCase)
            ? ZoomAudioMode.ProgramMix
            : ZoomAudioMode.PerGuestIso;

    // Deliberately asymmetric with Parse above: Parse reads hostile/legacy input
    // (a corrupted or future-version file) and must stay permissive with the
    // product default. Format writes operator intent to disk, so it is TOTAL — a switch
    // over every enum member with no fallback arm — so a third topology added to
    // ZoomAudioMode without a matching case here is a compiler warning, not a
    // silent "programMix" write.
    public static string Format(ZoomAudioMode mode) => mode switch
    {
        ZoomAudioMode.ProgramMix => ProgramMixValue,
        ZoomAudioMode.PerGuestIso => PerGuestIsoValue,
    };
}
