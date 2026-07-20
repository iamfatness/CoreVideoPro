using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MasteringPresetLibraryTests
{
    private static MasteringSettings Settings(double glueAmount = 0.4) =>
        MasteringPresetCatalog.Neutral with { GlueAmount = glueAmount };

    [Fact]
    public void Save_AddsANamedPresetWithFreshId()
    {
        var result = MasteringPresetLibrary.Save([], "  Sunday Show ", Settings());

        Assert.NotNull(result);
        var (presets, saved) = result.Value;
        Assert.Single(presets);
        Assert.Equal("Sunday Show", saved.Name);       // trimmed
        Assert.StartsWith("user-", saved.Id, StringComparison.Ordinal);
        Assert.Equal(0.4, saved.Settings.GlueAmount);
    }

    [Fact]
    public void Save_SameNameOverwritesInPlaceKeepingTheId()
    {
        var first = MasteringPresetLibrary.Save([], "Show", Settings(0.2))!.Value;
        var second = MasteringPresetLibrary.Save(first.Presets, "show", Settings(0.9));

        Assert.NotNull(second);
        var (presets, saved) = second.Value;
        Assert.Single(presets);                          // overwrite, not duplicate
        Assert.Equal(first.Saved.Id, saved.Id);          // stable id across saves
        Assert.Equal(0.9, presets[0].Settings.GlueAmount);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("Streaming")]   // built-ins are immutable and reserved
    [InlineData("neutral")]
    [InlineData("  Podcast  ")]
    public void Save_RejectsEmptyAndBuiltInNames(string? name)
    {
        Assert.Null(MasteringPresetLibrary.Save([], name, Settings()));
    }

    [Fact]
    public void Save_RejectsWhenLibraryIsFull()
    {
        IReadOnlyList<MasteringUserPreset> presets = [];
        for (var i = 0; i < MasteringPresetLibrary.MaxPresets; i++)
        {
            presets = MasteringPresetLibrary.Save(presets, $"Preset {i}", Settings())!.Value.Presets;
        }

        Assert.Null(MasteringPresetLibrary.Save(presets, "One Too Many", Settings()));
        // ...but overwriting an existing name still works at capacity.
        Assert.NotNull(MasteringPresetLibrary.Save(presets, "Preset 0", Settings(0.7)));
    }

    [Fact]
    public void Rename_ChangesTheNameOnly()
    {
        var (presets, saved) = MasteringPresetLibrary.Save([], "Old Name", Settings(0.3))!.Value;

        var renamed = MasteringPresetLibrary.Rename(presets, saved.Id, "New Name");

        Assert.NotNull(renamed);
        Assert.Equal("New Name", renamed.Value.Renamed.Name);
        Assert.Equal(saved.Id, renamed.Value.Renamed.Id);
        Assert.Equal(0.3, renamed.Value.Presets[0].Settings.GlueAmount);
    }

    [Fact]
    public void Rename_RejectsUnknownIdBuiltInAndDuplicateNames()
    {
        var one = MasteringPresetLibrary.Save([], "One", Settings())!.Value;
        var two = MasteringPresetLibrary.Save(one.Presets, "Two", Settings())!.Value;

        Assert.Null(MasteringPresetLibrary.Rename(two.Presets, "user-missing", "Fine"));
        Assert.Null(MasteringPresetLibrary.Rename(two.Presets, one.Saved.Id, "Broadcast"));
        Assert.Null(MasteringPresetLibrary.Rename(two.Presets, one.Saved.Id, "two"));
        // Renaming to its own name (case change) is allowed.
        Assert.NotNull(MasteringPresetLibrary.Rename(two.Presets, one.Saved.Id, "ONE"));
    }

    [Fact]
    public void Delete_RemovesOnlyTheTargetPreset()
    {
        var one = MasteringPresetLibrary.Save([], "One", Settings())!.Value;
        var two = MasteringPresetLibrary.Save(one.Presets, "Two", Settings())!.Value;

        var deleted = MasteringPresetLibrary.Delete(two.Presets, one.Saved.Id);

        Assert.NotNull(deleted);
        var remaining = Assert.Single(deleted.Value.Presets);
        Assert.Equal("Two", remaining.Name);
        Assert.Null(MasteringPresetLibrary.Delete(deleted.Value.Presets, "user-missing"));
    }

    [Fact]
    public void PersistedRoundTrip_PreservesEveryField()
    {
        var settings = new MasteringSettings(
            true, 2, 0.45, -1.0, 8.0, 1.5, 70.0, 18000.0, -1.0, 1.5, 0.5, 1.1, true,
            3.0, 20.0, 300.0, 1.0, true, -2.0, 0.5, 1.0);

        var roundTripped = MasteringPresetLibrary.FromPersisted(MasteringPresetLibrary.ToPersisted(settings));

        Assert.Equal(settings, roundTripped);
    }

    [Fact]
    public void FromPersisted_ClampsOutOfRangeValuesToTheRackLimits()
    {
        var hostile = new PersistedMasteringSettings
        {
            Enabled = true,
            TargetIndex = 9,
            CeilingDbfs = 3.0,       // above full scale — would defeat the ceiling
            GlueRatio = 100.0,
            StereoWidth = 5.0,
            GlueAttackMs = 0.0,
            MaxRideDb = 99.0
        };

        var restored = MasteringPresetLibrary.FromPersisted(hostile);

        Assert.Equal(2, restored.TargetIndex);
        Assert.Equal(-0.5, restored.CeilingDbfs);
        Assert.Equal(10.0, restored.GlueRatio);
        Assert.Equal(2.0, restored.StereoWidth);
        Assert.Equal(1.0, restored.GlueAttackMs);
        Assert.Equal(12.0, restored.MaxRideDb);
    }

    [Fact]
    public void FromPersistedPresets_DropsUnusableEntriesLoudlyNotTheWholeLoad()
    {
        var restored = MasteringPresetLibrary.FromPersistedPresets(
        [
            new PersistedMasteringPreset { Id = "user-1", Name = "Keep Me" },
            new PersistedMasteringPreset { Id = "", Name = "No Id" },
            new PersistedMasteringPreset { Id = "user-2", Name = "  " },
            new PersistedMasteringPreset { Id = "user-3", Name = "Streaming" },  // built-in collision
            new PersistedMasteringPreset { Id = "user-4", Name = "keep me" }     // duplicate name
        ]);

        var kept = Assert.Single(restored);
        Assert.Equal("Keep Me", kept.Name);
    }
}
