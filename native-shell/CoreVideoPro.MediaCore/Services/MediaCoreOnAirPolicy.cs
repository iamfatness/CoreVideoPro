using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Decides, from the profile the core sent at handshake, whether an on-air action may
/// proceed. A real core always starts from the stub module set and swaps in each real
/// adapter that constructs. When one does not, the stub stays: a no-op compositor that
/// reports live frames and draws nothing, or a counting encoder that reports a recording
/// and writes no file. The core records each failure in <c>capabilityStates</c>; nothing
/// in the shell read it (#762). These checks are the shell half of the refusal; the core
/// refuses the matching command as well.
///
/// Each method returns null when the action may proceed, otherwise the sentence the
/// operator sees. A missing profile is not a refusal here: the handshake path already
/// blocks every command until the profile arrives.
/// </summary>
public static class MediaCoreOnAirPolicy
{
    /// <summary>Engine on, and anything that puts pixels on Program.</summary>
    public static string? EngineBlockReason(NativeMediaCoreProfile? profile)
    {
        if (profile is null)
        {
            return null;
        }

        if (profile.Renderer.Equals("software", StringComparison.OrdinalIgnoreCase) ||
            !profile.HasAvailableCapability("gpu-compositor"))
        {
            return "The media core has no GPU compositor" + Detail(profile, "gpu-compositor") +
                   ". Program would show nothing, so Engine stays off. Check the GPU and its driver, then restart CoreVideo Pro.";
        }

        return null;
    }

    /// <summary>Record. Includes the Engine check: a recording of nothing is still nothing.</summary>
    public static string? RecordBlockReason(NativeMediaCoreProfile? profile)
    {
        if (EngineBlockReason(profile) is { } engine)
        {
            return engine;
        }

        if (profile is not null && IsFailed(profile, "program-recording"))
        {
            return "The media core has no recording encoder" + Detail(profile, "program-recording") +
                   ". Record would report a recording and write no file, so it is refused. Check Media Foundation on this machine, then restart CoreVideo Pro.";
        }

        return null;
    }

    /// <summary>Join. A core with no Zoom engine used to join a two-person stub meeting (#741).</summary>
    public static string? JoinBlockReason(NativeMediaCoreProfile? profile)
    {
        if (profile is null || profile.CapabilityStates.Count == 0)
        {
            return null;
        }

        if (!profile.HasAvailableCapability("zoom-raw-video"))
        {
            return "The media core has no Zoom engine" + Detail(profile, "zoom-raw-video") +
                   ". Join is refused. Reinstall or repair the Zoom runtime, then restart CoreVideo Pro.";
        }

        return null;
    }

    private static bool IsFailed(NativeMediaCoreProfile profile, string capability) =>
        profile.CapabilityStates.TryGetValue(capability, out var state) &&
        string.Equals(state.State, "failed-to-construct", StringComparison.OrdinalIgnoreCase);

    private static string Detail(NativeMediaCoreProfile profile, string capability)
    {
        if (!profile.CapabilityStates.TryGetValue(capability, out var state))
        {
            return "";
        }

        var text = string.IsNullOrWhiteSpace(state.Detail) ? state.State : $"{state.State}: {state.Detail}";
        return $" ({text})";
    }
}
