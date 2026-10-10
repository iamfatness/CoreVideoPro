using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SourceLowerThirdTextTests
{
    [Fact]
    public void SavedBlankSuppressesAllMetadataFallbackUntilExplicitReset()
    {
        var names = new Dictionary<string, string>(); var secondary = new Dictionary<string, string>();
        Assert.Equal(("Guest", "Guest"), SourceLowerThirdText.Resolve(secondary, "zoom:a", "Guest", "Guest"));
        Assert.True(SourceLowerThirdText.Apply(names, secondary, "zoom:a", "Alice", "  "));
        Assert.Equal(("", ""), SourceLowerThirdText.Resolve(secondary, "zoom:a", "Presenter", "New role"));
        Assert.False(SourceLowerThirdText.Apply(names, secondary, "zoom:a", "Alice", ""));
        Assert.True(SourceLowerThirdText.Apply(names, secondary, "zoom:a", "Alice", null));
        Assert.Equal(("Presenter", "New role"), SourceLowerThirdText.Resolve(secondary, "zoom:a", "Presenter", "New role"));
    }

    [Fact]
    public void RealStoreRestartPreservesBlankCustomTextAndUnrelatedPreferences()
    {
        var folder = Path.Combine(Path.GetTempPath(), "corevideo-lt-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            new FileProductionOutputPreferencesStore(folder).Save(new ProductionOutputPreferences
            {
                SourceSecondaryLines = new() { ["zoom:a"] = "", ["capture:camera"] = "Organization / example.com" },
                SourceDisplayNames = new() { ["zoom:a"] = "Alice" }, StreamRtmpEnabled = false
            });
            var loaded = new FileProductionOutputPreferencesStore(folder).Load()!;
            Assert.Equal("", loaded.SourceSecondaryLines["zoom:a"]);
            Assert.Equal("Organization / example.com", loaded.SourceSecondaryLines["capture:camera"]);
            Assert.Equal("Alice", loaded.SourceDisplayNames["zoom:a"]);
            Assert.False(loaded.StreamRtmpEnabled);
        }
        finally { Directory.Delete(folder, true); }
    }

    [Fact]
    public void ReusedSlotLoadsNewSourceTextWithoutInheritingPreviousGuestsBlankOverride()
    {
        var names = new Dictionary<string, string>(); var secondary = new Dictionary<string, string>();
        var slot = new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "a" };
        var saves = 0;
        var editor = new ShowInputSlotViewModel(slot, () => { },
            resolveDisplayName: (id, fallback) => ShowInputRosterService.ResolveDisplayName(names, id, fallback),
            resolveSecondary: (id, fallback) => id is not null && secondary.TryGetValue(id, out var value) ? value : fallback,
            usesDefaultSecondary: id => id is null || !secondary.ContainsKey(id),
            applyText: (id, name, text) => { SourceLowerThirdText.Apply(names, secondary, id!, name, text); saves++; });
        editor.RefreshSourceOptions([new Participant { Id = "a", Name = "Alice" }, new Participant { Id = "b", Name = "Bob", Title = "Reader" }], []);
        editor.ApplyLowerThirdText("Alice Smith", "", false);
        Assert.Equal(1, saves);
        Assert.Equal("", editor.SecondaryLine);
        Assert.False(editor.UsesDefaultSecondaryLine);
        slot.ParticipantId = "b";
        Assert.Equal("Bob", editor.DisplayName);
        Assert.True(editor.UsesDefaultSecondaryLine);
        Assert.Contains("Reader", editor.SecondaryLine);
        slot.ParticipantId = "a";
        Assert.Equal("Alice Smith", editor.DisplayName);
        Assert.Equal("", editor.SecondaryLine);
    }
}
