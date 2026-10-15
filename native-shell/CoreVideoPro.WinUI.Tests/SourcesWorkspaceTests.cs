using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SourcesWorkspaceTests
{
    [Fact]
    public void InspectionFiltersAndColumnsNeverChangeAssignmentsOrLiveSelections()
    {
        var writes = 0;
        var model = new ShowInputSlot { SlotNumber = 2, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "alice", InShow = true };
        var input = new ShowInputSlotViewModel(model, () => writes++, onIsoToggled: (_, _) => writes++);
        input.RefreshSourceOptions([new Participant { Id = "alice", Name = "Alice" }], []);
        input.SetIsoSelected(true);
        var state = new SourcesWorkspaceState();
        state.Inspect(input); state.Query = "not alice"; state.Category = "Cameras";
        state.Columns = new() { ShowAudio = false }; state.InspectorOpen = false;
        Assert.False(state.Matches(input));
        Assert.Same(input, state.SelectedInput);
        Assert.Equal("zoom:alice", input.SourceId);
        Assert.True(input.InShow); Assert.True(input.IsoEnabled);
        Assert.Equal(0, writes);
        state.Query = "02"; state.Category = "Zoom"; Assert.True(state.Matches(input));
    }

    [Theory]
    [InlineData(ShowInputKind.SrtIngest, "Network")]
    [InlineData(ShowInputKind.RtmpIngest, "Network")]
    [InlineData(ShowInputKind.Ndi, "Network")]
    [InlineData(ShowInputKind.Blackmagic, "Cameras")]
    [InlineData(ShowInputKind.Aja, "Cameras")]
    [InlineData(ShowInputKind.Screen, "Screens")]
    [InlineData(ShowInputKind.Browser, "Browser")]
    [InlineData(ShowInputKind.Unassigned, "Unassigned")]
    public void CategoryFilterRetainsAllExistingAdapterKinds(ShowInputKind kind, string category)
    {
        var input = new ShowInputSlotViewModel(new ShowInputSlot { SlotNumber = 7, Kind = kind }, () => { });
        var state = new SourcesWorkspaceState { Category = category };
        Assert.True(state.Matches(input)); state.Category = "Zoom"; Assert.False(state.Matches(input));
    }

    [Fact]
    public void OfflineNameAndSelectionStayBoundToTheirOwnSource()
    {
        var input = new ShowInputSlotViewModel(new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "alice", InShow = true }, () => { });
        input.RefreshSourceOptions([new Participant { Id = "alice", Name = "Alice" }], []);
        var state = new SourcesWorkspaceState(); state.Inspect(input);
        input.RefreshSourceOptions([], []);
        Assert.Contains("Alice", input.OverviewName); Assert.Equal("zoom:alice", input.SourceId);
        Assert.Same(input, state.SelectedInput); state.Query = "Alice"; Assert.True(state.Matches(input));
        input.Unassign(); Assert.DoesNotContain("Alice", input.OverviewName);
        Assert.Same(input, state.SelectedInput);
    }

    [Fact]
    public void MeasuredCellsSeparateFormatAndFpsWithoutTreatingStaleOrRequestedCapsAsLive()
    {
        var row = new MultiviewInputRow(1) { FormatLabel = "1920×1080@59.94", ConfiguredCapLabel = "up to 1080p requested" };
        Assert.Equal("1920×1080", row.ResolutionLabel); Assert.Equal("59.94", row.FrameRateLabel);
        row.FormatStale = true; Assert.Equal("—", row.FrameRateLabel);
        Assert.Equal("1920×1080@59.94", row.FormatLabel);
        row.FormatLabel = "—"; row.FormatStale = false; Assert.Equal("—", row.FrameRateLabel);
    }

    [Fact]
    public void ColumnPreferencesRoundTripSeparatelyFromProductionSettingsAndMissingFieldsDefaultVisible()
    {
        var folder = Path.Combine(Path.GetTempPath(), "corevideo-sources-layout-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var path = Path.Combine(folder, "layout.json"); var store = new SourcesWorkspacePreferenceStore(path);
            Assert.Equal(new SourcesWorkspacePreferences(), store.Load());
            var value = new SourcesWorkspacePreferences { ShowAudio = false, ShowType = false };
            store.Save(value); Assert.Equal(value, new SourcesWorkspacePreferenceStore(path).Load());
            File.WriteAllText(path, "{}"); Assert.True(store.Load().ShowSignal);
            File.WriteAllText(path, "bad json"); Assert.Equal(new SourcesWorkspacePreferences(), store.Load());
        }
        finally { foreach (var file in Directory.GetFiles(folder)) File.Delete(file); Directory.Delete(folder); }
    }
}
