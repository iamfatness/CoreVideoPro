using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.MagicScene;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MagicSceneBindingPolicyTests
{
    [Fact]
    public void BindsDistinctGuestsToPreviewRoutesAndPreservesProgramCopy()
    {
        var program = new List<SourceRoute>
        {
            new() { Id = "left", Mode = SourceRouteMode.Fixed, ParticipantId = "old-a", ZIndex = 0 },
            new() { Id = "right", Mode = SourceRouteMode.Fixed, ParticipantId = "old-b", ZIndex = 1 }
        };
        var preview = program.Select(route => route.Clone()).ToList();
        var result = MagicSceneBindingPolicy.Apply(preview,
        [
            new NativeDirectorSlotBinding { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" },
            new NativeDirectorSlotBinding { SlotIndex = 1, PersonId = "b", SourceId = "zoom:b" }
        ], out var reason);
        Assert.True(result, reason);
        Assert.Equal(["a", "b"], preview.Select(route => route.ParticipantId).ToArray());
        Assert.Equal(["old-a", "old-b"], program.Select(route => route.ParticipantId).ToArray());
    }

    [Fact]
    public void RefusesDuplicatePersonWithoutChangingPreviewRoutes()
    {
        var preview = new List<SourceRoute>
        {
            new() { Id = "left", Mode = SourceRouteMode.Fixed, ParticipantId = "old-a", ZIndex = 0 },
            new() { Id = "right", Mode = SourceRouteMode.Fixed, ParticipantId = "old-b", ZIndex = 1 }
        };
        var result = MagicSceneBindingPolicy.Apply(preview,
        [
            new NativeDirectorSlotBinding { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" },
            new NativeDirectorSlotBinding { SlotIndex = 1, PersonId = "a", SourceId = "zoom:a" }
        ], out var reason);
        Assert.False(result);
        Assert.Contains("duplicate", reason);
        Assert.Equal(["old-a", "old-b"], preview.Select(route => route.ParticipantId).ToArray());
    }
}
