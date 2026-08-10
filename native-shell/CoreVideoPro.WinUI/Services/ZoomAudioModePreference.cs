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
/// anything this cannot read becomes <see cref="ZoomAudioMode.ProgramMix"/>, the
/// long-standing Z1 topology, rather than silently putting a show on air with
/// zoom-mix stripped off the program buses.
/// </summary>
public static class ZoomAudioModePreference
{
    public const string ProgramMixValue = "programMix";
    public const string PerGuestIsoValue = "perGuestIso";

    public static ZoomAudioMode Parse(string? persisted) =>
        string.Equals(persisted?.Trim(), PerGuestIsoValue, StringComparison.OrdinalIgnoreCase)
            ? ZoomAudioMode.PerGuestIso
            : ZoomAudioMode.ProgramMix;

    public static string Format(ZoomAudioMode mode) =>
        mode == ZoomAudioMode.PerGuestIso ? PerGuestIsoValue : ProgramMixValue;
}
