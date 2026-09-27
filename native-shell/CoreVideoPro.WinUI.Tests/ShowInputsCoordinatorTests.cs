using System.Collections.ObjectModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Characterization tests for <see cref="ShowInputsCoordinator"/> — the ShowInputs roster /
/// assignment / auto-assign / ISO-selection cluster extracted from the StudioViewModel god object
/// (PR3 strangler). These are the FIRST-EVER coverage of the roster→editor projection: they exist
/// because the coordinator is constructible in isolation (a real <see cref="InMemoryShowInputRosterStore"/>
/// + a fake <see cref="IShowInputsHost"/> + a fake <see cref="IMediaCoreBridge"/>), which
/// StudioViewModel itself is not. They lock the golden-path behavior — the 0xc000027b signature
/// gate, auto-assign of free slots, operator unassign, and the ISO × ShowInputs integration
/// (ISO-4) surviving a roster refresh — BEFORE anyone refactors it further.
/// </summary>
public sealed class ShowInputsCoordinatorTests
{
    [Fact]
    public void ReusedZoomIdClearsOldFormatAndRejectsOldSourceFactWithoutTake()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "42";
            host.ShowInputs[0].InShow = true;
        }
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 1,
            Participants = [new RawParticipantEvent { UserId = "42", DisplayName = "First", VideoOn = true, SourceGeneration = 1 }]
        });
        var row = coordinator.MultiviewInputRows.Rows[0];
        bridge.EmitFormat(new ZoomSourceFormatFact
        {
            ParticipantId = "42", RosterEpoch = "1:1:test", SourceGeneration = 1,
            Width = 1920, Height = 1080, Fps = 30, FrameId = 1, FrameAtMs = 100
        });
        Assert.Equal("1920×1080@30", row.FormatLabel);
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 2,
            Participants = [new RawParticipantEvent { UserId = "42", DisplayName = "Second", VideoOn = true, SourceGeneration = 2 }],
            ZoomSubscriptions = [new ZoomMediaSpineSubscription
            {
                ParticipantId = "42", SourceGeneration = 1, Kind = "participant-video",
                DeliveredWidth = 1920, DeliveredHeight = 1080, DeliveredFps = 30,
                LastFrameAtMs = 100, LastFrameAgeMs = 0
            }]
        });
        Assert.Equal("CONNECTING", row.StatusLabel);
        Assert.Equal("—", row.FormatLabel);
        Assert.Equal(2, row.LastAppliedSourceInstance?.Generation);
        bridge.EmitFormat(new ZoomSourceFormatFact
        {
            ParticipantId = "42", RosterEpoch = "1:1:test", SourceGeneration = 1,
            Width = 1920, Height = 1080, Fps = 30, FrameId = 2, FrameAtMs = 200
        });
        Assert.Equal("—", row.FormatLabel);
        bridge.EmitFormat(new ZoomSourceFormatFact
        {
            ParticipantId = "42", RosterEpoch = "1:1:test", SourceGeneration = 2,
            Width = 1280, Height = 720, Fps = 30, FrameId = 1, FrameAtMs = 201
        });
        Assert.Equal("1280×720@30", row.FormatLabel);
        Assert.Equal(0, bridge.SyncCalls);
    }

    [Fact]
    public void TileCropFillsTheWallPanelBeforeClippingOneTile()
    {
        var crop = SourceFramingLayoutService.ResolveTileCrop(
            78, 43, new Windows.Foundation.Rect(0.25, 0.5, 0.25, 0.5));
        var transform = SwapChainPanelTransformPolicy.Resolve(
            crop.Width, crop.Height, 1920, 1080, fillPanelForTileCrop: true);
        Assert.Equal(0, transform.OffsetX);
        Assert.Equal(0, transform.OffsetY);
        Assert.InRange(transform.ScaleX, 0.1624f, 0.1626f);
        Assert.InRange(transform.ScaleY, 0.0795f, 0.0797f);
        var ordinary = SwapChainPanelTransformPolicy.Resolve(
            crop.Width, crop.Height, 1920, 1080, fillPanelForTileCrop: false);
        Assert.Equal(ordinary.ScaleX, ordinary.ScaleY);
        Assert.NotEqual(ordinary.ScaleX, transform.ScaleX);
    }

    [Fact]
    public void VersionedBridgeVideoOffPatchesFixedRowWithoutSceneSyncOrTake()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[1].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[1].ParticipantId = "guest-2";
            host.ShowInputs[1].InShow = true;
        }
        coordinator.InitializeShowInputEditors();
        var rows = coordinator.MultiviewInputRows.Rows;
        var row = rows[1];
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 7,
            Participants = [new RawParticipantEvent { UserId = "guest-2", DisplayName = "Guest", VideoOn = true }],
            ZoomSubscriptions = [new ZoomMediaSpineSubscription
            {
                ParticipantId = "guest-2", Kind = "participant-video", DeliveredWidth = 1920,
                DeliveredHeight = 1080, DeliveredFps = 30, LastFrameAtMs = 1000, LastFrameAgeMs = 0
            }]
        });
        Assert.Equal("LIVE", row.StatusLabel);
        Assert.Equal("1920×1080@30", row.FormatLabel);
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 8,
            Participants = [new RawParticipantEvent { UserId = "guest-2", DisplayName = "Guest", VideoOn = false }]
        });
        Assert.Same(rows, coordinator.MultiviewInputRows.Rows);
        Assert.Same(row, rows[1]);
        Assert.Equal(10, rows.Count);
        Assert.Equal("VIDEO OFF", row.StatusLabel);
        Assert.Equal("—", row.FormatLabel);
        bridge.EmitMultiview(new MultiviewSharedTexture
        {
            Tiles = [new MultiviewTile { Role = "source", Slot = 1, SourceId = "zoom:guest-2",
                X = 0.2, Y = 0.2, W = 0.2, H = 0.25 }]
        });
        Assert.False(row.HasPreviewTile);
        Assert.Equal(8, row.HighestObservationRevision);
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 7,
            Participants = [new RawParticipantEvent { UserId = "guest-2", DisplayName = "Guest", VideoOn = true }],
            ZoomSubscriptions = [new ZoomMediaSpineSubscription
            {
                ParticipantId = "guest-2", Kind = "participant-video", DeliveredWidth = 1920,
                DeliveredHeight = 1080, DeliveredFps = 30, LastFrameAtMs = 1000, LastFrameAgeMs = 0
            }]
        });
        Assert.Equal("VIDEO OFF", row.StatusLabel);
        Assert.Equal("—", row.FormatLabel);
        Assert.Equal(0, bridge.SyncCalls);
    }

    [Fact]
    public void FormatFactPatchesSameRowWithoutSnapshotOrSceneSyncAndRejectsOldEpoch()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "guest";
            host.ShowInputs[0].InShow = true;
        }
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 1,
            Participants = [new RawParticipantEvent { UserId = "guest", DisplayName = "Guest", VideoOn = true }]
        });
        var rows = coordinator.MultiviewInputRows.Rows;
        var row = rows[0];
        Assert.Equal("CONNECTING", row.StatusLabel);
        bridge.EmitFormat(new ZoomSourceFormatFact
        {
            ParticipantId = "guest", RosterEpoch = "1:1:test", Width = 1280,
            Height = 720, Fps = 24, FrameId = 2, FrameAtMs = 250
        });
        Assert.Same(rows, coordinator.MultiviewInputRows.Rows);
        Assert.Equal("1280×720@24", row.FormatLabel);
        Assert.Equal("LIVE", row.StatusLabel);
        Assert.Equal(0, bridge.SyncCalls);

        // The next snapshot cannot erase a newer direct fact or roll it back.
        bridge.EmitRoster(new NativeMediaCoreStateSnapshot
        {
            MeetingState = "in_meeting", RosterEpoch = "1:1:test", RosterRevision = 2,
            Participants = [new RawParticipantEvent { UserId = "guest", DisplayName = "Guest", VideoOn = true }],
            ZoomSubscriptions = [new ZoomMediaSpineSubscription
            {
                ParticipantId = "guest", Kind = "participant-video", DeliveredWidth = 640,
                DeliveredHeight = 360, DeliveredFps = 15, LastFrameAtMs = 100, LastFrameAgeMs = 50
            }]
        });
        Assert.Equal("1280×720@24", row.FormatLabel);
        bridge.EmitFormat(new ZoomSourceFormatFact
        {
            ParticipantId = "guest", RosterEpoch = "old", Width = 1920,
            Height = 1080, Fps = 60, FrameId = 3, FrameAtMs = 500
        });
        Assert.Equal("1280×720@24", row.FormatLabel);
        bridge.EmitHealth(new MediaCoreHealth { Stopped = true });
        Assert.Equal("—", row.FormatLabel);
    }

    [Fact]
    public void DeliveredFormatAndAgePatchWithoutRosterRevisionThenEngineOffWipesOldMeeting()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "guest";
            host.ShowInputs[0].InShow = true;
        }
        var row = coordinator.MultiviewInputRows.Rows[0];
        void Emit(string epoch, long revision, int width, int height, int fps, int frameId, double atMs, double ageMs) =>
            bridge.EmitRoster(new NativeMediaCoreStateSnapshot
            {
                MeetingState = "in_meeting", RosterEpoch = epoch, RosterRevision = revision,
                Participants = [new RawParticipantEvent { UserId = "guest", DisplayName = "Guest", VideoOn = true }],
                ZoomSubscriptions = [new ZoomMediaSpineSubscription
                {
                    ParticipantId = "guest", Kind = "participant-video", DeliveredWidth = width,
                    DeliveredHeight = height, DeliveredFps = fps, LastFrameId = frameId,
                    LastFrameAtMs = atMs, LastFrameAgeMs = ageMs
                }]
            });
        Emit("1:1:test", 7, 1920, 1080, 30, 1, 1000, 100);
        Assert.Equal("1920×1080@30", row.FormatLabel);
        Assert.Equal("", row.ConfiguredCapLabel);
        Emit("1:1:test", 7, 1280, 720, 15, 2, 1500, 200);
        Assert.Equal("1280×720@15", row.FormatLabel);
        Assert.Equal("up to 1080p requested", row.ConfiguredCapLabel);
        Assert.Equal("LIVE", row.StatusLabel);
        Emit("1:1:test", 7, 1280, 720, 15, 2, 1500, 1501);
        Assert.Equal("STALLED", row.StatusLabel);
        Assert.True(row.FormatStale);
        Emit("1:1:test", 7, 1280, 720, 15, 2, 1500, 200);
        Assert.Equal("STALLED", row.StatusLabel);

        bridge.EmitHealth(new MediaCoreHealth { Stopped = true });
        Assert.Equal("IDLE", row.StatusLabel);
        Assert.Equal("—", row.FormatLabel);
        Assert.Equal(-1, row.FrameAgeMs);
        using (ShowInputWriteScope.Enter("test"))
            host.ShowInputs[0].InShow = false;
        Assert.Equal("—", row.FormatLabel);
        using (ShowInputWriteScope.Enter("test"))
            host.ShowInputs[0].InShow = true;
        Assert.Equal("IDLE", row.StatusLabel);
        Emit("1:1:test", 8, 1280, 720, 15, 3, 2000, 10);
        Assert.Equal("IDLE", row.StatusLabel);
        Emit("1:2:test", 1, 640, 360, 30, 1, 100, 30);
        Assert.Equal("640×360@30", row.FormatLabel);
        Assert.Equal("1:2:test", row.RosterEpoch);
        Assert.Same(row, coordinator.MultiviewInputRows.Rows[0]);
    }

    [Fact]
    public void IsoLifecycleFactsPatchRecWithoutSelectingPreviewOrTaking()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "guest";
            host.ShowInputs[0].InShow = true;
        }
        var row = coordinator.MultiviewInputRows.Rows[0];
        Assert.Equal("OFF", row.RecLabel);
        bridge.EmitIso(new IsoOutputLifecycleFact("zoom:guest", "session-1", "armed", 1));
        Assert.Equal("ARMED", row.RecLabel);
        bridge.EmitIso(new IsoOutputLifecycleFact("zoom:guest", "session-1", "recording", 2));
        Assert.Equal("RECORDING", row.RecLabel);
        Assert.Equal("REC", row.RecChipLabel);
        bridge.EmitIso(new IsoOutputLifecycleFact("zoom:guest", "session-1", "armed", 1));
        Assert.Equal("RECORDING", row.RecLabel);
        bridge.EmitIso(new IsoOutputLifecycleFact("zoom:guest", "session-1", "error", 3));
        Assert.Equal("ERROR", row.RecLabel);
        bridge.EmitIso(new IsoOutputLifecycleFact("zoom:guest", "session-1", "off", 4));
        Assert.Equal("OFF", row.RecLabel);
        Assert.Equal(0, bridge.SyncCalls);
    }

    [Theory]
    [InlineData("preparing", "healthy", "writing", "armed")]
    [InlineData("producing", "healthy", "writing", "recording")]
    [InlineData("producing", "healthy", "warning", "error")]
    [InlineData("interrupted", "failed", "writing", "error")]
    [InlineData("completed", "healthy", "writing", "off")]
    public void IsoChipUsesObservedWriterLifecycle(string state, string health, string streamStatus, string expected)
    {
        var recording = new NativeMediaCoreRecordingSession
        {
            SessionId = "session", Status = "recording", WriterStatus = "writing",
            TargetFolder = "temp", FilenamePrefix = "test", Format = "mp4", Quality = "high", ProgramPath = "program.mp4",
            Lifecycle = new CoreVideoPro.MediaCore.Contracts.OutputLifecycle
            {
                SessionId = "session", State = state, Health = health,
                DesiredActive = true, Finalized = state == "completed"
            },
            Streams = [new NativeMediaCoreRecordingStream
            {
                Kind = "iso", SourceId = "zoom:guest", Path = "iso.mp4", Status = streamStatus
            }]
        };
        var observed = IsoOutputLifecyclePolicy.Observe(recording);
        Assert.Equal(expected == "off" ? null : expected,
            observed.GetValueOrDefault("zoom:guest").State);
    }

    [Fact]
    public void ExistingMultiviewTileCoordinatesPatchPreviewCropAndTally()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "guest";
            host.ShowInputs[0].InShow = true;
        }
        var row = coordinator.MultiviewInputRows.Rows[0];
        bridge.EmitMultiview(new MultiviewSharedTexture
        {
            Tiles = [new MultiviewTile
            {
                Role = "source", SourceId = "zoom:guest", Slot = 0,
                X = 0.2, Y = 0.5, W = 0.2, H = 0.25, Tally = "pvw"
            }]
        });
        Assert.True(row.HasPreviewTile);
        Assert.InRange(row.PreviewCropRect.X, 0.1999, 0.2001);
        Assert.Equal(0.25, row.PreviewCropRect.Height);
        Assert.Equal("pvw", row.PreviewTally);
        var crop = SourceFramingLayoutService.ResolveTileCrop(72, 40, row.PreviewCropRect);
        Assert.InRange(crop.Width, 359.99, 360.01);
        Assert.InRange(crop.Height, 159.99, 160.01);
        Assert.InRange(crop.TranslateX, -72.01, -71.99);
        Assert.InRange(crop.TranslateY, -80.01, -79.99);
        bridge.EmitMultiview(new MultiviewSharedTexture { Tiles = [] });
        Assert.False(row.HasPreviewTile);
        Assert.Equal("none", row.PreviewTally);
        Assert.Equal(0, bridge.SyncCalls);
    }

    [Fact]
    public void SpeakerOnlyTileFactMovesTheInspectorBorderWithoutSceneSync()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[0].Kind = ShowInputKind.ZoomParticipant;
            host.ShowInputs[0].ParticipantId = "guest";
            host.ShowInputs[0].InShow = true;
        }
        void Emit(bool talking) => bridge.EmitMultiview(new MultiviewSharedTexture
        {
            Tiles = [new MultiviewTile
            {
                Role = "source", SourceId = "zoom:guest", Slot = 0,
                X = 0.2, Y = 0.5, W = 0.2, H = 0.25, ActiveSpeaker = talking
            }]
        });
        Emit(false);
        var row = coordinator.MultiviewInputRows.Rows[0];
        Assert.Equal("none", row.PreviewTally);
        Emit(true);
        Assert.Equal("talking", row.PreviewTally);
        Emit(false);
        Assert.Equal("none", row.PreviewTally);
        Assert.Equal(0, bridge.SyncCalls);
    }

    [Theory]
    [InlineData("pgm", false, false, "pgm")]
    [InlineData("pvw", false, false, "pvw")]
    [InlineData("none", true, false, "talking")]
    [InlineData("pgm", true, true, "hold")]
    public void PreviewTallyHonorsProgramPreviewTalkingAndStall(
        string tally, bool talking, bool stalled, string expected)
    {
        Assert.Equal(expected, MultiviewTileCropPolicy.Tally(
            new MultiviewTile { Tally = tally, ActiveSpeaker = talking }, stalled));
    }

    [Fact]
    public void InspectorTilePatchSkipsIdenticalFrameRectsButAdmitsTallyChange()
    {
        var old = new[] { new MultiviewTile
        {
            Role = "source", Slot = 0, SourceId = "zoom:guest", X = 0.2, Y = 0.5,
            W = 0.2, H = 0.25, Tally = "none"
        } };
        var same = new[] { new MultiviewTile
        {
            Role = "source", Slot = 0, SourceId = "zoom:guest", X = 0.2, Y = 0.5,
            W = 0.2, H = 0.25, Tally = "none"
        } };
        var changed = new[] { new MultiviewTile
        {
            Role = "source", Slot = 0, SourceId = "zoom:guest", X = 0.2, Y = 0.5,
            W = 0.2, H = 0.25, Tally = "pvw"
        } };
        Assert.True(MultiviewTileCropPolicy.SameTiles(old, same));
        Assert.False(MultiviewTileCropPolicy.SameTiles(old, changed));
    }

    [Fact]
    public void ProgramTallyChangesStatusChipWithoutReplacingSourceHealth()
    {
        var row = new MultiviewInputRow(6) { StatusLabel = "LIVE", PreviewTally = "pgm" };
        Assert.Equal("PGM", row.StatusChipLabel);
        Assert.Equal("LIVE", row.StatusLabel);
        row.PreviewTally = "none";
        Assert.Equal("LIVE", row.StatusChipLabel);
    }

    [Fact]
    public void CaptureSignalFactPatchesHostUvcFormatWithoutRowClick()
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(bridge, new InMemoryShowInputRosterStore(), host);
        using (ShowInputWriteScope.Enter("test"))
        {
            host.ShowInputs[5].Kind = ShowInputKind.UvcWebcam;
            host.ShowInputs[5].CaptureDeviceId = "cam";
            host.ShowInputs[5].InShow = true;
        }
        var row = coordinator.MultiviewInputRows.Rows[5];
        var camera = new CaptureDevice
        {
            Id = "cam", NativeDeviceId = "cam", Vendor = "test", Name = "Host cam",
            Inputs = [], SelectedInputId = "cam"
        };
        host.CaptureDevices.Add(camera);
        camera.ObservedFrameWidth = 1920;
        camera.ObservedFrameHeight = 1080;
        camera.ObservedFrameRate = 60;
        camera.ConnectionState = CaptureConnectionState.Connected;
        camera.SignalPresent = true;
        Assert.Equal("capture:cam", row.SourceId);
        Assert.Equal("1920×1080@60", row.FormatLabel);
        Assert.Equal("LIVE", row.StatusLabel);
        bridge.EmitMultiview(new MultiviewSharedTexture
        {
            Tiles = [new MultiviewTile { Role = "source", Slot = 5, SourceId = "capture:cam",
                X = 0.5, Y = 0.5, W = 0.2, H = 0.25, Tally = "pgm" }]
        });
        Assert.Equal("PGM", row.StatusChipLabel);
        Assert.Equal("pgm", row.PreviewTally);
        bridge.EmitHealth(new MediaCoreHealth { Stopped = true });
        Assert.Equal("—", row.FormatLabel);
        Assert.Equal("IDLE", row.StatusLabel);
        Assert.Equal(0, bridge.SyncCalls);
    }

    private static (ShowInputsCoordinator Coordinator, FakeShowInputsHost Host) Build()
    {
        var host = new FakeShowInputsHost();
        var coordinator = new ShowInputsCoordinator(new FakeMediaCoreBridge(), new InMemoryShowInputRosterStore(), host);
        coordinator.InitializeShowInputEditors();
        return (coordinator, host);
    }

    // ---------------------------------------------------------------- signature gating

    [Fact]
    public void RefreshEditors_SameIdSet_SkipsRebuild_ChangedIdSet_DiffUpdatesInPlace()
    {
        var (coordinator, host) = Build();
        var editorsInstance = coordinator.ShowInputEditors;
        var slotCount = editorsInstance.Count;
        Assert.Equal(host.ShowInputs.Count, slotCount);

        // Initialize already forced one projection. A no-op refresh with the SAME id-set must skip
        // past the signature gate (no readout/readiness pokes) — the 0xc000027b "never rebuild a
        // bound collection at snapshot rate" rule.
        var readinessBefore = host.NotifyShowReadinessCount;
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore, host.NotifyShowReadinessCount);

        // Change the id-set the picker depends on → the signature changes → the projection runs
        // (readiness poke fires), diff-updating the SAME collection instance IN PLACE (never
        // replaced; count stable).
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore + 1, host.NotifyShowReadinessCount);
        Assert.Same(editorsInstance, coordinator.ShowInputEditors);
        Assert.Equal(slotCount, coordinator.ShowInputEditors.Count);

        // A second refresh with that same (now-current) id-set skips again.
        coordinator.RefreshShowInputEditors();
        Assert.Equal(readinessBefore + 1, host.NotifyShowReadinessCount);
    }

    // ---------------------------------------------------------------- auto-assign

    [Fact]
    public void ReapplyAutoAssign_FillsAFreeSlot_WithARoomParticipant_WhenEnabled()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];

        coordinator.ReapplyShowInputAutoAssign();

        var assigned = host.ShowInputs.FirstOrDefault(slot => slot.Kind == ShowInputKind.ZoomParticipant);
        Assert.NotNull(assigned);
        Assert.Equal("p1", assigned!.ParticipantId);
        Assert.True(assigned.InShow);
    }

    [Fact]
    public void ReapplyAutoAssign_Disabled_DoesNotAssignFreeSlots()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = false;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];

        coordinator.ReapplyShowInputAutoAssign();

        Assert.DoesNotContain(host.ShowInputs, slot => slot.Kind == ShowInputKind.ZoomParticipant);
    }

    // ---------------------------------------------------------------- unassign

    [Fact]
    public void UnassignShowInput_ClearsTheSlot()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();

        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        Assert.True(editor.IsAssigned);

        coordinator.UnassignShowInput(editor);

        Assert.False(editor.IsAssigned);
        Assert.Equal(ShowInputKind.Unassigned, editor.Kind);
        Assert.DoesNotContain(host.ShowInputs, slot => slot.Kind == ShowInputKind.ZoomParticipant);
    }

    // The owner's live sequence, 2026-09-12, end to end through the COORDINATOR -
    // not the roster-service leaf. A leaf test cannot catch the regression that
    // actually matters here (dropping the reserved-slots argument at the call
    // site), which is the #481 lesson: test the whole decision.
    //
    //   13:52  operator unassigns slot 4 (mimoLive)
    //   14:00  "LIVE | Admin" joins  ->  roster-sync puts them straight into slot 4
    [Fact]
    public void ARosterSyncNeverRefillsASlotTheOperatorUnassigned()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;

        // Fill every slot, so the slot the operator clears is the FIRST free one -
        // the only arrangement that can reproduce the defect.
        host.RoomParticipantsForInputs = Enumerable
            .Range(0, host.ShowInputs.Count)
            .Select(index => new Participant { Id = $"p{index}", Name = $"Guest {index}" })
            .ToList();
        coordinator.ReapplyShowInputAutoAssign();

        var cleared = coordinator.ShowInputEditors.First(e => e.SlotNumber == 4);
        var evictedId = cleared.ParticipantId;
        Assert.NotNull(evictedId);
        coordinator.UnassignShowInput(cleared);
        Assert.Equal(ShowInputKind.Unassigned, host.ShowInputs.First(s => s.SlotNumber == 4).Kind);

        // A newcomer joins while everyone else stays.
        var roster = host.RoomParticipantsForInputs
            .Where(participant => participant.Id != evictedId)
            .Select(participant => Context(participant.Id))
            .Append(Context("p-newcomer"))
            .ToList();

        coordinator.SyncShowInputsFromMeeting(roster);

        var slot4 = host.ShowInputs.First(s => s.SlotNumber == 4);
        Assert.Equal(ShowInputKind.Unassigned, slot4.Kind);
        Assert.Null(slot4.ParticipantId);
        Assert.DoesNotContain(host.ShowInputs, slot => slot.ParticipantId == "p-newcomer");
    }

    // Owner ruling: sticky until the MEETING ROSTER EMPTIES - not for the app session.
    [Fact]
    public void AnEmptyRosterReleasesTheOperatorsClearedSlots()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = Enumerable
            .Range(0, host.ShowInputs.Count)
            .Select(index => new Participant { Id = $"p{index}", Name = $"Guest {index}" })
            .ToList();
        coordinator.ReapplyShowInputAutoAssign();
        coordinator.UnassignShowInput(coordinator.ShowInputEditors.First(e => e.SlotNumber == 4));

        // The meeting ends - every slot frees, and the reservation is released.
        coordinator.SyncShowInputsFromMeeting([]);

        // A new meeting fills every slot. Slot 4 is an ordinary slot again, so it
        // takes a guest like any other; while reserved it would have stayed empty
        // and one guest would have had nowhere to go.
        coordinator.SyncShowInputsFromMeeting(
            Enumerable.Range(0, host.ShowInputs.Count).Select(index => Context($"q{index}")).ToList());

        var slot4 = host.ShowInputs.First(s => s.SlotNumber == 4);
        Assert.Equal(ShowInputKind.ZoomParticipant, slot4.Kind);
        Assert.False(string.IsNullOrEmpty(slot4.ParticipantId));
    }

    private static CoreVideoPro.MediaCore.Services.LiveProductionSync.LiveProductionParticipantContext Context(string id) =>
        new() { Id = id, Name = id, RoleLabel = "guest" };

    // ---------------------------------------------------------------- ISO × ShowInputs integration

    [Fact]
    public void IsoSelection_SurvivesARosterRefresh_ViaInPlaceReProjection()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();

        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        Assert.True(editor.ShowIsoToggle);
        var sourceId = editor.SourceId;
        Assert.False(string.IsNullOrEmpty(sourceId));

        // Operator toggles ISO on for this source (the TwoWay-bound checkbox → the editor's
        // IsoEnabled setter fires the coordinator's OnShowInputIsoToggled callback, which records
        // the persisted selection).
        editor.IsoEnabled = true;
        Assert.Contains(sourceId!, coordinator.IsoSelectedSourceIds);
        Assert.True(editor.IsoEnabled);

        // Simulate a row losing its ISO flag (as a wholesale rebuild would). The signature-gated
        // roster refresh re-projects the PERSISTED selection back onto the row IN PLACE
        // (ApplyIsoSelectionToEditors) — the ISO × ShowInputs integration must survive roster
        // churn so the isoSourceIds recording selection is never silently dropped.
        editor.SetIsoSelected(false);
        Assert.False(editor.IsoEnabled);

        // A second guest joins → the participant id-set signature changes → the projection runs.
        // p1 is still present, so its row keeps its source (not source-missing).
        host.RoomParticipantsForInputs =
        [
            new Participant { Id = "p1", Name = "Guest 1" },
            new Participant { Id = "p2", Name = "Guest 2" }
        ];
        coordinator.RefreshShowInputEditors();

        Assert.True(editor.IsoEnabled); // re-projected from the persisted selection
        Assert.Contains(sourceId!, coordinator.IsoSelectedSourceIds);

        // And the eligible-present projection that feeds recording still resolves it.
        Assert.Contains(sourceId!, coordinator.ComputeEligiblePresentIsoSourceIds());
    }

    // ---------------------------------------------------------------- persistence: immediate save + load-once
    // Regression cover for the 2026-08-09 live-meeting roster-revert defect. Two of the
    // mechanisms that let a webcam stomp survive: (a) the roster save rode ONLY the
    // coalesced Low-priority ApplyShowInputRefresh, so a crash (four that day) lost the
    // operator's pending change and the relaunch restored the pre-change roster; (b) any
    // future second LoadShowInputRoster would overwrite live operator changes with disk
    // state. The coordinator now saves synchronously on every editor-observed slot change
    // and refuses a second load.

    private static (ShowInputsCoordinator Coordinator, FakeShowInputsHost Host, InMemoryShowInputRosterStore Store) BuildWithStore()
    {
        var host = new FakeShowInputsHost();
        var store = new InMemoryShowInputRosterStore();
        var coordinator = new ShowInputsCoordinator(new FakeMediaCoreBridge(), store, host);
        coordinator.LoadShowInputRoster();
        coordinator.InitializeShowInputEditors();
        return (coordinator, host, store);
    }

    [Fact]
    public void OperatorAssignment_PersistsImmediately_WithoutTheCoalescedRefresh()
    {
        var (coordinator, host, store) = BuildWithStore();
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.RefreshShowInputEditors();

        var editor = coordinator.ShowInputEditors[0];
        editor.SelectedUnifiedSourceId = "zoom:p1";
        editor.InShow = true;

        // The store must already hold the operator's change — NOTHING else ran (the fake
        // host's OnShowInputChanged is a no-op, standing in for the coalesced dispatcher
        // callback a crash would lose).
        var saved = store.Load();
        Assert.NotNull(saved);
        var slot1 = saved!.Slots.First(record => record.SlotNumber == 1);
        Assert.Equal(ShowInputKind.ZoomParticipant, slot1.Kind);
        Assert.Equal("p1", slot1.ParticipantId);
        Assert.True(slot1.InShow);
    }

    [Fact]
    public void OperatorUnassign_PersistsImmediately()
    {
        var (coordinator, host, store) = BuildWithStore();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();

        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        coordinator.UnassignShowInput(editor);

        var saved = store.Load();
        Assert.NotNull(saved);
        Assert.DoesNotContain(saved!.Slots, record => record.Kind == ShowInputKind.ZoomParticipant);
    }

    [Fact]
    public void AutoAssign_PersistsImmediately()
    {
        var (coordinator, host, store) = BuildWithStore();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];

        coordinator.ReapplyShowInputAutoAssign();

        // The auto-assign wrote the model directly; the editor VMs observe the model and
        // the coordinator saves synchronously — so a crash right after auto-assign no
        // longer loses the roster either.
        var saved = store.Load();
        Assert.NotNull(saved);
        var assigned = saved!.Slots.FirstOrDefault(record => record.Kind == ShowInputKind.ZoomParticipant);
        Assert.NotNull(assigned);
        Assert.Equal("p1", assigned!.ParticipantId);
    }

    [Fact]
    public void SecondLoad_IsRefused_AndNeverOverwritesLiveOperatorChanges()
    {
        var (coordinator, host, store) = BuildWithStore();
        host.RoomParticipantsForInputs = [new Participant { Id = "p2", Name = "Guest 2" }];
        coordinator.RefreshShowInputEditors();

        // Operator assigns slot 1 live.
        coordinator.ShowInputEditors[0].SelectedUnifiedSourceId = "zoom:p2";

        // Stale disk state appears (another instance / an old file): a webcam in slot 1.
        store.Save(new ShowInputRosterSnapshot
        {
            Slots =
            [
                new ShowInputSlotRecord
                {
                    SlotNumber = 1,
                    Kind = ShowInputKind.UvcWebcam,
                    CaptureDeviceId = "cam-1",
                    InShow = true
                }
            ]
        });

        // Persisted state restores ONLY at startup — a second load must refuse, keeping
        // the operator's live assignment.
        coordinator.LoadShowInputRoster();

        Assert.Equal(ShowInputKind.ZoomParticipant, host.ShowInputs[0].Kind);
        Assert.Equal("p2", host.ShowInputs[0].ParticipantId);
        Assert.Null(host.ShowInputs[0].CaptureDeviceId);
    }

    [Fact]
    public void BuildIsoSourceTargets_Empty_WhenMasterSwitchOff()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.RoomParticipantsForInputs = [new Participant { Id = "p1", Name = "Guest 1" }];
        coordinator.ReapplyShowInputAutoAssign();
        var editor = coordinator.ShowInputEditors.First(e => e.Kind == ShowInputKind.ZoomParticipant);
        editor.IsoEnabled = true;

        host.IsoRecordingEnabled = false;
        Assert.Empty(coordinator.BuildIsoSourceTargets().SourceIds);

        host.IsoRecordingEnabled = true;
        // needs the eligible-present projection refreshed so the row is not source-missing
        coordinator.RefreshShowInputEditors(force: true);
        Assert.Contains(editor.SourceId!, coordinator.BuildIsoSourceTargets().SourceIds);
    }

    [Fact]
    public void BuildIsoSourceTargets_VideoOffGuestDoesNotConsumeEightSourceCap()
    {
        var (coordinator, host) = Build();
        host.AutomationAutoAssignInputsEnabled = true;
        host.IsoRecordingEnabled = true;
        host.RoomParticipantsForInputs = Enumerable.Range(1, 9)
            .Select(index => new Participant
            {
                Id = $"p{index}",
                Name = $"Guest {index}",
                Health = index == 1 ? FeedHealth.VideoOff : FeedHealth.Live
            })
            .ToList();

        coordinator.ReapplyShowInputAutoAssign();
        foreach (var editor in coordinator.ShowInputEditors.Where(editor => editor.Kind == ShowInputKind.ZoomParticipant))
        {
            editor.IsoEnabled = true;
        }

        var targets = coordinator.BuildIsoSourceTargets().SourceIds;

        Assert.Equal(8, targets.Count);
        Assert.DoesNotContain("zoom:p1", targets);
        Assert.Equal(Enumerable.Range(2, 8).Select(index => $"zoom:p{index}"), targets);
    }

    // ---------------------------------------------------------------- fakes

    private sealed class FakeShowInputsHost : IShowInputsHost
    {
        public ObservableCollection<ShowInputSlot> ShowInputs { get; } =
            new(ShowInputRosterService.CreateDefaultSlots());

        public IReadOnlyList<Participant> RoomParticipantsForInputs { get; set; } = [];

        public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

        public ObservableCollection<AudioCaptureDevice> AudioCaptureDevices { get; } = [];

        public IReadOnlyList<MediaBinGroup> MediaBinGroups { get; set; } = [];

        public bool IsoRecordingEnabled { get; set; }

        public bool AutomationAutoAssignInputsEnabled { get; set; }

        public ObservableCollection<SrtIngestSource> SrtIngestSources { get; } = [];

        public bool CanAddSrtIngestSource => SrtIngestSources.Count < MaxSrtIngestSources;

        public int MaxSrtIngestSources => 8;

        public int NotifyShowReadinessCount { get; private set; }

        public string? LastCommandStatus { get; private set; }

        public SrtIngestSource CreateSrtIngestSource(int number) =>
            new() { Id = $"srt-source-{number:00}", Number = number };

        public void RemoveVirtualSrtIngestDevice(string deviceId) { }

        public string CommandStatus { set => LastCommandStatus = value; }

        public void OnShowInputChanged() { }

        public void SetCaptureDeviceAudioSource(string? captureDeviceId, string? audioDeviceId) { }

        public string ResolveSourceDisplayName(string? sourceId, string derivedName) => derivedName;

        public void SetSourceDisplayName(string? sourceId, string? name) { }

        public void EnsureAssignedScreensConnected() { }

        public void NotifyShowReadinessChanged() => NotifyShowReadinessCount++;

        public void RefreshIsoReadouts() { }

        public void RaiseShowInputReadoutsChanged() { }

        public void RefreshMultiviewGridTiles() { }

        public void RefreshPreviewRoutingState() { }

        public void RefreshDualCaptureSourceOptions() { }

        public void RefreshCaptureFleetSummary() { }

        public Task TrySyncMediaCoreAsync() => Task.CompletedTask;

        public void SaveProductionOutputPreferences() { }

        public void RunOnUiThread(Action action) => action();
    }

    private sealed class FakeMediaCoreBridge : IMediaCoreBridge
    {
        public int SyncCalls { get; private set; }
        public void EmitRoster(NativeMediaCoreStateSnapshot snapshot) => SnapshotChanged?.Invoke(snapshot);
        public void EmitFormat(ZoomSourceFormatFact fact) => ZoomSourceFormatReceived?.Invoke(fact);
        public void EmitHealth(MediaCoreHealth health) => HealthChanged?.Invoke(health);
        public void EmitIso(IsoOutputLifecycleFact fact) => OutputLifecycleChanged?.Invoke(fact);
        public void EmitMultiview(MultiviewSharedTexture texture) => MultiviewSharedTextureReceived?.Invoke(texture);
        public bool Running => true;

        public NativeMediaCoreProfile? Profile => null;

        public string ProfileSummary => "GPU 1080p60";

        public NativeMediaCoreStateSnapshot? LastSnapshot => null;

#pragma warning disable CS0067 // events are part of the seam surface; the coordinator does not raise them
        public event Action<MediaCoreHealth>? HealthChanged;
        public event Action<string>? StatusChanged;
        public event Action<NativeMediaCoreProfile>? ProfileChanged;
        public event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
        public event Action<IsoOutputLifecycleFact>? OutputLifecycleChanged;
        public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
        public event Action<ZoomSourceFormatFact>? ZoomSourceFormatReceived;
        public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
        public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
        public event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
        public event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
        public event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;
#pragma warning restore CS0067

        public void ConfigureZoomSpineSync(Func<CancellationToken, Task<Dictionary<string, object?>>>? payloadFactory) { }

        public Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<NativeMediaCoreProfile?>(null);

        public void Stop() { }

        public Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(new NativeMediaCoreStateSnapshot());

        public Task<NativeMediaCoreStateSnapshot> SyncAsync(
            IReadOnlyList<NativeMediaCoreCommand> commands, double? elapsedMs = null,
            CancellationToken cancellationToken = default)
        {
            SyncCalls++;
            throw new NotSupportedException();
        }

        public Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetVstParamAsync(string selection, long paramId, double normalized,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task SetVstStateAsync(string selection, string stateBase64,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RegisterCaptureShmAsync(string deviceId, string shmName, int width, int height,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default, string? outputSourceId = null) =>
            throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default) =>
            Task.FromResult<IReadOnlyList<NativeCaptureDeviceStatus>>([]);

        public Task AddBrowserSourceAsync(string url, int width, int height, int fps,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
