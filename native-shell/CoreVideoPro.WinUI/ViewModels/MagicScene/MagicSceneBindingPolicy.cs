using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.MagicScene;

/// <summary>Applies the kernel's person binds to Preview route models only.</summary>
public static class MagicSceneBindingPolicy
{
    public static bool Apply(
        IList<SourceRoute> previewRoutes,
        IReadOnlyList<NativeDirectorSlotBinding> bindings,
        out string reason)
    {
        if (bindings.Count == 0)
        {
            reason = "No guest video or host camera has a content frame; Preview has no person to bind.";
            return false;
        }
        var seats = previewRoutes
            .Where(route => route.Mode is not SourceRouteMode.ScreenShare)
            .OrderBy(route => route.ZIndex)
            .ToList();
        if (bindings.Count > seats.Count)
        {
            reason = $"Preview scene has {seats.Count} person seats for {bindings.Count} recommended sources.";
            return false;
        }
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var usedSlots = new HashSet<int>();
        foreach (var binding in bindings)
        {
            if (binding.SlotIndex < 0 || binding.SlotIndex >= seats.Count ||
                !usedSlots.Add(binding.SlotIndex) ||
                string.IsNullOrWhiteSpace(binding.PersonId) ||
                !seen.Add(binding.PersonId) ||
                !(binding.SourceId.StartsWith("zoom:", StringComparison.Ordinal) && binding.SourceId.Length > 5 ||
                  binding.SourceId.StartsWith("capture:", StringComparison.Ordinal) && binding.SourceId.Length > 8))
            {
                reason = "Director returned an empty, duplicate, or unroutable person slot.";
                return false;
            }
        }
        // Validate the whole recommendation before changing a single Preview route.
        foreach (var seat in seats)
        {
            seat.Mode = SourceRouteMode.None;
            seat.ParticipantId = null;
            seat.CaptureDeviceId = null;
            seat.ShowInputSlotNumber = null;
            seat.ProductionRoleId = null;
            seat.SpotlightIndex = null;
        }
        foreach (var binding in bindings)
        {
            var seat = seats[binding.SlotIndex];
            if (binding.SourceId.StartsWith("zoom:", StringComparison.Ordinal))
            {
                seat.Mode = SourceRouteMode.Fixed;
                seat.ParticipantId = binding.SourceId[5..];
            }
            else
            {
                seat.Mode = SourceRouteMode.CaptureDevice;
                seat.CaptureDeviceId = binding.SourceId[8..];
            }
        }
        reason = string.Empty;
        return true;
    }
}
