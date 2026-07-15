using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ShowInputRosterServiceTests
{
    [Fact]
    public void SwapSlotAssignments_SwapsFullContentAcrossKinds()
    {
        // Zoom participant <-> UVC capture: Kind's clearing setter must not eat the ids.
        var zoomSlot = new ShowInputSlot { SlotNumber = 1 };
        zoomSlot.Kind = ShowInputKind.ZoomParticipant;
        zoomSlot.ParticipantId = "guest-42";
        zoomSlot.InShow = true;

        var uvcSlot = new ShowInputSlot { SlotNumber = 2 };
        uvcSlot.Kind = ShowInputKind.UvcWebcam;
        uvcSlot.CaptureDeviceId = "cam-hd60";
        uvcSlot.AudioDeviceId = "mic-1";
        uvcSlot.InShow = false;

        ShowInputRosterService.SwapSlotAssignments(zoomSlot, uvcSlot);

        Assert.Equal(1, zoomSlot.SlotNumber); // slot numbers never move — content does
        Assert.Equal(ShowInputKind.UvcWebcam, zoomSlot.Kind);
        Assert.Equal("cam-hd60", zoomSlot.CaptureDeviceId);
        Assert.Equal("mic-1", zoomSlot.AudioDeviceId);
        Assert.Null(zoomSlot.ParticipantId);
        Assert.False(zoomSlot.InShow);

        Assert.Equal(2, uvcSlot.SlotNumber);
        Assert.Equal(ShowInputKind.ZoomParticipant, uvcSlot.Kind);
        Assert.Equal("guest-42", uvcSlot.ParticipantId);
        Assert.Null(uvcSlot.CaptureDeviceId);
        Assert.Null(uvcSlot.AudioDeviceId);
        Assert.True(uvcSlot.InShow);
    }

    [Fact]
    public void SwapSlotAssignments_AssignedWithUnassignedMovesTheAssignment()
    {
        var assigned = new ShowInputSlot { SlotNumber = 3 };
        assigned.Kind = ShowInputKind.ZoomParticipant;
        assigned.ParticipantId = "guest-7";
        assigned.InShow = true;

        var empty = new ShowInputSlot { SlotNumber = 4 };

        ShowInputRosterService.SwapSlotAssignments(assigned, empty);

        Assert.Equal(ShowInputKind.Unassigned, assigned.Kind);
        Assert.Null(assigned.ParticipantId);
        Assert.False(assigned.InShow);

        Assert.Equal(ShowInputKind.ZoomParticipant, empty.Kind);
        Assert.Equal("guest-7", empty.ParticipantId);
        Assert.True(empty.InShow);
    }

    [Fact]
    public void SwapSlotAssignments_SameSlotIsANoOp()
    {
        var slot = new ShowInputSlot { SlotNumber = 5 };
        slot.Kind = ShowInputKind.UvcWebcam;
        slot.CaptureDeviceId = "cam-a";
        slot.InShow = true;

        ShowInputRosterService.SwapSlotAssignments(slot, slot);

        Assert.Equal(ShowInputKind.UvcWebcam, slot.Kind);
        Assert.Equal("cam-a", slot.CaptureDeviceId);
        Assert.True(slot.InShow);
    }

    [Fact]
    public void CaptureDeviceFormatLabel_StaysPendingUntilRealFramesArrive()
    {
        var device = Device("cam-uvc", "USB Capture", "uvc", 0, 0, 0);

        Assert.Equal("Format pending", device.ResolutionLabel);
        Assert.Equal("Format pending", device.FormatLabel);
        Assert.False(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Detected, device.ConnectionState);

        device.ApplyFormatTelemetry(1920, 1080, 60);

        Assert.Equal("1920x1080", device.ResolutionLabel);
        Assert.Equal("1920x1080 · 60 fps", device.FormatLabel);
        Assert.False(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Detected, device.ConnectionState);

        device.ApplyFrameTelemetry(1920, 1080, 30);

        Assert.Equal("1920x1080", device.ResolutionLabel);
        Assert.Equal("1920x1080 · 60 fps", device.FormatLabel);
        Assert.True(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Connected, device.ConnectionState);
    }

    [Fact]
    public void CaptureDeviceFormatLabel_UsesFirstLiveFrameWhenDiscoveryFormatIsMissing()
    {
        var device = Device("cam-uvc", "USB Capture", "uvc", 0, 0, 0);

        device.ApplyFrameTelemetry(1280, 720, 30);

        Assert.Equal("1280x720", device.ResolutionLabel);
        Assert.Contains("1280x720", device.FormatLabel, StringComparison.Ordinal);
        Assert.Contains("30 fps", device.FormatLabel, StringComparison.Ordinal);
        Assert.Equal(1280, device.Width);
        Assert.Equal(720, device.Height);
        Assert.Equal(30, device.FrameRate);
        Assert.True(device.SignalPresent);
        Assert.Equal(CaptureConnectionState.Connected, device.ConnectionState);
    }

    [Fact]
    public void CaptureDeviceFormatLabel_UsesObservedFramesWhenDeclaredFormatIsIncomplete()
    {
        var device = Device("cam-uvc", "USB Capture", "uvc", 0, 0, 0);

        device.ApplyObservedFrameTelemetry(1920, 1080, 60);

        Assert.Equal("1920x1080", device.ResolutionLabel);
        Assert.Contains("1920x1080", device.FormatLabel, StringComparison.Ordinal);
        Assert.Contains("60 fps", device.FormatLabel, StringComparison.Ordinal);
    }

    [Fact]
    public void BuildSourceOptions_IncludesWindowsAndUvcWebcams()
    {
        var options = ShowInputRosterService.BuildSourceOptions(
            ShowInputKind.UvcWebcam,
            [],
            [
                Device("cam-windows", "Integrated Camera", "windows", 1920, 1080, 30),
                Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60),
                Device("decklink", "DeckLink Mini", "blackmagic", 1920, 1080, 60)
            ]);

        Assert.Equal(["cam-windows", "cam-uvc"], options.Select(option => option.Value));
    }

    [Fact]
    public void BuildCaptureSourceOptions_IncludesFormatAndConnectionState()
    {
        var options = ShowInputRosterService.BuildCaptureSourceOptions(
            [
                Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true),
                Device("cam-windows", "Integrated Camera", "windows", 1280, 720, 30)
            ]);

        Assert.Equal(string.Empty, options[0].Value);
        Assert.Equal("Choose capture source", options[0].Label);
        Assert.Equal("cam-uvc", options[1].Value);
        Assert.Contains("USB Capture", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("1920x1080", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("60 fps", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("connected", options[1].Label, StringComparison.Ordinal);
        Assert.Equal("cam-windows", options[2].Value);
    }

    [Fact]
    public void BuildAudioSourceOptions_LabelsSourceKindAndDriver()
    {
        var options = ShowInputRosterService.BuildAudioSourceOptions(
            [
                AudioDevice("mic-01", "USB Microphone", "wasapi-input", "WASAPI"),
                AudioDevice("asio-01", "Focusrite USB ASIO", "asio-input", "ASIO")
            ]);

        Assert.Equal(string.Empty, options[0].Value);
        Assert.Contains("USB Microphone", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("Mic / line input", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("WASAPI", options[1].Label, StringComparison.Ordinal);
        Assert.Contains("Focusrite USB ASIO", options[2].Label, StringComparison.Ordinal);
        Assert.Contains("ASIO", options[2].Label, StringComparison.Ordinal);
    }

    [Fact]
    public void CreateEmbeddedCaptureAudioDevices_AddsBlackmagicAndAjaAudioSources()
    {
        var audioDevices = AudioCaptureDeviceDiscoveryService.CreateEmbeddedCaptureAudioDevices(
            [
                Device("decklink", "DeckLink Mini Recorder", "blackmagic", 1920, 1080, 60),
                Device("aja", "AJA U-TAP", "aja", 1920, 1080, 60),
                Device("uvc", "USB Webcam", "uvc", 1280, 720, 30)
            ]);

        Assert.Equal(2, audioDevices.Count);
        Assert.Contains(audioDevices, device =>
            device.LinkedCaptureDeviceId == "decklink" &&
            device.SourceKind == "embedded-capture-audio" &&
            device.DriverName == "Blackmagic DeckLink");
        Assert.Contains(audioDevices, device =>
            device.LinkedCaptureDeviceId == "aja" &&
            device.SourceKind == "embedded-capture-audio" &&
            device.DriverName == "AJA NTV2");
    }

    [Fact]
    public void BuildMultiviewTiles_UsesSelectedCaptureDeviceForUvcSlot()
    {
        var slots = new[]
        {
            new ShowInputSlot
            {
                SlotNumber = 1,
                Kind = ShowInputKind.UvcWebcam,
                CaptureDeviceId = "cam-uvc",
                InShow = true
            }
        };
        var devices = new[]
        {
            Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true)
        };

        var tiles = ShowInputRosterService.BuildMultiviewTiles(slots, [], devices, []);

        var tile = Assert.Single(tiles);
        Assert.Equal("capture:cam-uvc", tile.Participant.Id);
        Assert.StartsWith("USB Capture", tile.Participant.Name, StringComparison.Ordinal);
        Assert.Contains("1920x1080", tile.Participant.Name, StringComparison.Ordinal);
        Assert.Equal("UVC webcam", tile.Participant.Title);
        Assert.Equal(1, tile.SourceIndex);
        Assert.Equal("capture:cam-uvc", tile.Surface.SurfaceKey);
        Assert.Contains("connected", tile.Surface.DetailLine, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("signal present", tile.Surface.DetailLine, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void BuildMultiviewTiles_UsesLiveCaptureSurfaceWhenFramesArrive()
    {
        var slots = new[]
        {
            new ShowInputSlot
            {
                SlotNumber = 1,
                Kind = ShowInputKind.UvcWebcam,
                CaptureDeviceId = "cam-uvc",
                InShow = true
            }
        };
        var devices = new[]
        {
            Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true)
        };
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "capture:cam-uvc", "USB Capture")
            .WithPreviewPixels([0, 0, 0, 255], 1, 1);

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            slots,
            [],
            devices,
            [],
            new Dictionary<string, VideoSurfaceState>(StringComparer.Ordinal)
            {
                ["cam-uvc"] = surface
            });

        var tile = Assert.Single(tiles);
        Assert.True(tile.Surface.HasPreviewBitmap);
        Assert.Equal("capture:cam-uvc", tile.Surface.SurfaceKey);
        Assert.Equal("USB Capture - 1920x1080", tile.Surface.Title);
    }

    [Fact]
    public void BuildMultiviewTiles_AddsLiveUnassignedCaptureDeviceAsFallback()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "capture:elgato", "Game Capture HD60 S+")
            .WithPreviewPixels([0, 0, 0, 255], 1, 1);

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.Unassigned, InShow = true }],
            [],
            [Device("elgato", "Game Capture HD60 S+", "uvc", 1920, 1080, 60, connected: true)],
            [],
            new Dictionary<string, VideoSurfaceState>(StringComparer.Ordinal)
            {
                ["elgato"] = surface
            });

        var tile = Assert.Single(tiles);
        Assert.Equal("capture:elgato", tile.Participant.Id);
        Assert.Equal("UVC webcam", tile.Participant.Title);
        Assert.True(tile.Surface.HasPreviewBitmap);
        Assert.Contains("Live capture fallback", tile.Surface.DetailLine, StringComparison.Ordinal);
    }

    [Fact]
    public void BuildMultiviewTiles_DoesNotDuplicateAssignedLiveCaptureFallback()
    {
        var surface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Multiview, "capture:cam-uvc", "USB Capture")
            .WithPreviewPixels([0, 0, 0, 255], 1, 1);

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true)],
            [],
            new Dictionary<string, VideoSurfaceState>(StringComparer.Ordinal)
            {
                ["cam-uvc"] = surface
            });

        var tile = Assert.Single(tiles);
        Assert.Equal(1, tile.SourceIndex);
        Assert.Equal("capture:cam-uvc", tile.Participant.Id);
        Assert.DoesNotContain("fallback", tile.Surface.DetailLine, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SameMultiviewTileStructure_MatchesParticipantAndSlotOrderOnly()
    {
        var first = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60, connected: true)],
            []);

        var second = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: false)],
            []);

        Assert.True(ShowInputRosterService.SameMultiviewTileStructure(first, second));
        Assert.NotEqual(first[0].Surface.DetailLine, second[0].Surface.DetailLine);
    }

    [Fact]
    public void SameMultiviewTileStructure_ReturnsFalseWhenRosterChanges()
    {
        var first = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc", InShow = true }],
            [],
            [Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60)],
            []);

        var second = ShowInputRosterService.BuildMultiviewTiles(
            [new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-windows", InShow = true }],
            [],
            [
                Device("cam-uvc", "USB Capture", "uvc", 1280, 720, 60),
                Device("cam-windows", "Integrated Camera", "windows", 1920, 1080, 30)
            ],
            []);

        Assert.False(ShowInputRosterService.SameMultiviewTileStructure(first, second));
    }

    [Fact]
    public void RosterPersistence_RoundTripsSlotAssignments()
    {
        var store = new InMemoryShowInputRosterStore();
        var saved = ShowInputRosterService.CreateDefaultSlots().ToList();
        saved[0].Kind = ShowInputKind.ZoomParticipant;
        saved[0].ParticipantId = "participant-1";
        saved[0].InShow = true;
        saved[2].Kind = ShowInputKind.UvcWebcam;
        saved[2].CaptureDeviceId = "cam-uvc";
        saved[2].AudioDeviceId = "mic-01";
        saved[2].InShow = true;

        store.Save(ShowInputRosterSerializer.CaptureFrom(saved));

        // Reload into a fresh default roster (simulates a restart).
        var reloaded = ShowInputRosterService.CreateDefaultSlots();
        ShowInputRosterSerializer.ApplyTo(reloaded, store.Load());

        var first = reloaded[0];
        Assert.Equal(ShowInputKind.ZoomParticipant, first.Kind);
        Assert.Equal("participant-1", first.ParticipantId);
        Assert.True(first.InShow);

        var third = reloaded[2];
        Assert.Equal(ShowInputKind.UvcWebcam, third.Kind);
        Assert.Equal("cam-uvc", third.CaptureDeviceId);
        Assert.Equal("mic-01", third.AudioDeviceId);
        Assert.True(third.InShow);
    }

    [Fact]
    public void RosterPersistence_RoundTripsThroughJsonFile()
    {
        var folder = Path.Combine(Path.GetTempPath(), "cvp-roster-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new FileShowInputRosterStore(folder);
            var saved = ShowInputRosterService.CreateDefaultSlots().ToList();
            saved[4].Kind = ShowInputKind.Media;
            saved[4].ParticipantId = ShowInputRosterService.ToMediaSourceId("asset-7");
            saved[4].InShow = true;

            store.Save(ShowInputRosterSerializer.CaptureFrom(saved));

            // A brand-new store instance over the same folder simulates a process restart.
            var reloaded = ShowInputRosterService.CreateDefaultSlots();
            ShowInputRosterSerializer.ApplyTo(reloaded, new FileShowInputRosterStore(folder).Load());

            var media = reloaded[4];
            Assert.Equal(ShowInputKind.Media, media.Kind);
            Assert.Equal("media:asset-7", media.ParticipantId);
            Assert.True(media.InShow);
        }
        finally
        {
            if (Directory.Exists(folder))
            {
                Directory.Delete(folder, recursive: true);
            }
        }
    }

    [Fact]
    public void RosterPersistence_KeepsIntentWhenReferencedSourceMissing()
    {
        var store = new InMemoryShowInputRosterStore();
        var saved = ShowInputRosterService.CreateDefaultSlots().ToList();
        saved[0].Kind = ShowInputKind.UvcWebcam;
        saved[0].CaptureDeviceId = "cam-gone";
        saved[0].InShow = true;
        store.Save(ShowInputRosterSerializer.CaptureFrom(saved));

        var reloaded = ShowInputRosterService.CreateDefaultSlots();
        ShowInputRosterSerializer.ApplyTo(reloaded, store.Load());

        // Intent is preserved even though the device is absent from the live roster.
        var slot = reloaded[0];
        Assert.Equal(ShowInputKind.UvcWebcam, slot.Kind);
        Assert.Equal("cam-gone", slot.CaptureDeviceId);
        Assert.True(slot.IsAssigned);

        // The referenced device is no longer present, so the multiview surfaces no tile for it
        // rather than crashing — the assignment stays intact for when the device returns.
        var tiles = ShowInputRosterService.BuildMultiviewTiles(reloaded, [], [], []);
        Assert.Empty(tiles);
    }

    [Fact]
    public void RosterPersistence_IgnoresMalformedSnapshot()
    {
        Assert.Null(ShowInputRosterSerializer.Deserialize("{ not valid json"));
        Assert.Null(ShowInputRosterSerializer.Deserialize(null));

        var slots = ShowInputRosterService.CreateDefaultSlots();
        // Applying a null snapshot must be a no-op, not throw.
        ShowInputRosterSerializer.ApplyTo(slots, null);
        Assert.All(slots, slot => Assert.Equal(ShowInputKind.Unassigned, slot.Kind));
    }

    [Fact]
    public void BuildSourceOptions_ListsMediaAssetsForMediaKind()
    {
        var options = ShowInputRosterService.BuildSourceOptions(
            ShowInputKind.Media,
            [],
            [],
            [
                Media("asset-1", "Intro Bumper", "video"),
                Media("asset-2", "Lower Third Loop", "image")
            ]);

        Assert.Equal(["media:asset-1", "media:asset-2"], options.Select(option => option.Value));
        Assert.Contains("Intro Bumper", options[0].Label, StringComparison.Ordinal);
        Assert.Contains("video", options[0].Label, StringComparison.Ordinal);
    }

    [Fact]
    public void BuildMultiviewTiles_UsesAssignedMediaAssetForMediaSlot()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.Media;
        slots[0].ParticipantId = ShowInputRosterService.ToMediaSourceId("asset-1");
        slots[0].InShow = true;

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            slots,
            [],
            [],
            [],
            mediaAssets:
            [
                Media("asset-1", "Intro Bumper", "video", isPlaying: true)
            ]);

        var tile = Assert.Single(tiles);
        Assert.Equal(1, tile.SourceIndex);
        Assert.Equal("media:asset-1", tile.Participant.Id);
        Assert.Equal("Intro Bumper", tile.Participant.Name);
        Assert.Equal(FeedHealth.Live, tile.Participant.Health);
        Assert.Equal("media:asset-1", tile.Surface.SurfaceKey);
        Assert.Equal("video", tile.Surface.MediaAssetKind);
        Assert.True(tile.Surface.MediaAssetPlaying);
        Assert.Equal(1920, tile.Surface.FramingSourceWidth);
        Assert.Equal(1080, tile.Surface.FramingSourceHeight);
    }

    [Fact]
    public void ApplySlotRoute_ResolvesMediaSlotToFixedMediaRoute()
    {
        var slot = new ShowInputSlot
        {
            SlotNumber = 3,
            Kind = ShowInputKind.Media,
            ParticipantId = ShowInputRosterService.ToMediaSourceId("asset-9"),
            InShow = true
        };
        var route = new SourceRoute { Id = "scene-1-3", ShowInputSlotNumber = 3 };

        ShowInputRosterService.ApplySlotRoute(route, slot);

        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("media:asset-9", route.ParticipantId);
        Assert.Null(route.CaptureDeviceId);
        Assert.True(ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var assetId));
        Assert.Equal("asset-9", assetId);
    }

    [Fact]
    public void ApplySlotRoute_ResolvesCaptureSlotToCaptureRoute()
    {
        var slot = new ShowInputSlot
        {
            SlotNumber = 1,
            Kind = ShowInputKind.Blackmagic,
            CaptureDeviceId = "decklink-1",
            InShow = true
        };
        var route = new SourceRoute { Id = "scene-1-1", ShowInputSlotNumber = 1 };

        ShowInputRosterService.ApplySlotRoute(route, slot);

        Assert.Equal(SourceRouteMode.CaptureDevice, route.Mode);
        Assert.Equal("decklink-1", route.CaptureDeviceId);
        Assert.Null(route.ParticipantId);
    }

    [Fact]
    public void TryApplyStandaloneRoleFallback_UsesLocalCameraAndScreenWithoutZoom()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.UvcWebcam;
        slots[0].CaptureDeviceId = "camera-1";
        slots[0].InShow = true;
        slots[2].Kind = ShowInputKind.Screen;
        slots[2].CaptureDeviceId = "screen:0";
        slots[2].InShow = true;

        var speaker = new SourceRoute { Id = "speaker", Mode = SourceRouteMode.ActiveSpeaker };
        var slides = new SourceRoute { Id = "slides", Mode = SourceRouteMode.ScreenShare };

        Assert.True(ShowInputRosterService.TryApplyStandaloneRoleFallback(speaker, slots, []));
        Assert.Equal(SourceRouteMode.CaptureDevice, speaker.Mode);
        Assert.Equal("camera-1", speaker.CaptureDeviceId);
        Assert.Equal(1, speaker.ShowInputSlotNumber);

        Assert.True(ShowInputRosterService.TryApplyStandaloneRoleFallback(slides, slots, []));
        Assert.Equal(SourceRouteMode.CaptureDevice, slides.Mode);
        Assert.Equal("screen:0", slides.CaptureDeviceId);
        Assert.Equal(3, slides.ShowInputSlotNumber);
    }

    [Fact]
    public void TryApplyStandaloneRoleFallback_PreservesZoomRoleRoutesWhenMeetingExists()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.UvcWebcam;
        slots[0].CaptureDeviceId = "camera-1";
        slots[0].InShow = true;
        var route = new SourceRoute { Id = "speaker", Mode = SourceRouteMode.ActiveSpeaker };
        var participants = new[] { new Participant { Id = "zoom-1", Name = "Guest" } };

        Assert.False(ShowInputRosterService.TryApplyStandaloneRoleFallback(route, slots, participants));
        Assert.Equal(SourceRouteMode.ActiveSpeaker, route.Mode);
        Assert.Null(route.CaptureDeviceId);
        Assert.Null(route.ShowInputSlotNumber);
    }

    [Fact]
    public void BuildMultiviewTiles_KeepsZoomSlotInOwnPositionWhenParticipantHasNoLiveTile()
    {
        // Regression: the operator assigns a Zoom participant whose camera is momentarily off.
        // The participant is present in the in-room roster (what the Sources picker offers) but
        // has no live video tile yet. The slot must still surface in its own position as a
        // waiting placeholder - it must NOT be dropped (which would shift every later slot and
        // make the multiview disagree with the Sources screen).
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.ZoomParticipant;
        slots[0].ParticipantId = "p-camera-off";
        slots[0].InShow = true;
        slots[1].Kind = ShowInputKind.ZoomParticipant;
        slots[1].ParticipantId = "p-camera-on";
        slots[1].InShow = true;

        var participants = new[]
        {
            new Participant { Id = "p-camera-off", Name = "Camera Off Guest", Health = FeedHealth.VideoOff },
            new Participant { Id = "p-camera-on", Name = "Live Host", Health = FeedHealth.Live }
        };

        // MultiviewTiles (live video tiles) only contains the camera-on participant.
        var liveTile = new ParticipantSurfaceTile
        {
            Participant = participants[1],
            Surface = VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, "p-camera-on", "Live Host"),
            SourceIndex = 2
        };

        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            slots,
            participants,
            [],
            [liveTile]);

        Assert.Equal(2, tiles.Count);
        var slot1 = tiles.Single(tile => tile.SourceIndex == 1);
        Assert.Equal("p-camera-off", slot1.Participant.Id);
        Assert.Equal("Camera Off Guest", slot1.Participant.Name);
        var slot2 = tiles.Single(tile => tile.SourceIndex == 2);
        Assert.Equal("p-camera-on", slot2.Participant.Id);
    }

    [Fact]
    public void BuildMultiviewLayoutSources_MapsZoomCaptureAndMediaSlotsInOrder()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.ZoomParticipant;
        slots[0].ParticipantId = "p-1";
        slots[0].InShow = true;
        slots[1].Kind = ShowInputKind.UvcWebcam;
        slots[1].CaptureDeviceId = "cam-uvc";
        slots[1].InShow = true;
        slots[2].Kind = ShowInputKind.Media;
        slots[2].ParticipantId = ShowInputRosterService.ToMediaSourceId("asset-7");
        slots[2].InShow = true;
        // Assigned but NOT in show — must be excluded.
        slots[3].Kind = ShowInputKind.ZoomParticipant;
        slots[3].ParticipantId = "p-2";
        slots[3].InShow = false;

        var participants = new[]
        {
            new Participant { Id = "p-1", Name = "Host", Health = FeedHealth.Live },
            new Participant { Id = "p-2", Name = "Guest", Health = FeedHealth.Live }
        };
        var devices = new[] { Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true) };
        var media = new[] { Media("asset-7", "Bumper", "video") };

        var sources = ShowInputRosterService.BuildMultiviewLayoutSources(slots, participants, devices, media);

        Assert.Equal(3, sources.Count);

        Assert.Equal("zoom", sources[0].Kind);
        Assert.Equal("p-1", sources[0].ParticipantId);
        Assert.Equal("zoom:p-1", sources[0].SourceId);
        Assert.Equal("Host", sources[0].Label);
        Assert.Equal(0, sources[0].Slot);

        Assert.Equal("capture", sources[1].Kind);
        Assert.Equal("cam-uvc", sources[1].CaptureDeviceId);
        Assert.Equal("capture:cam-uvc", sources[1].SourceId);

        Assert.Equal("media", sources[2].Kind);
        Assert.Equal("asset-7", sources[2].MediaAssetId);
        Assert.Equal("media:asset-7", sources[2].SourceId);
    }

    [Fact]
    public void ResolveGridShape_PicksReasonableColumnsRows()
    {
        Assert.Equal((1, 1), ShowInputRosterService.ResolveGridShape(1));
        Assert.Equal((2, 1), ShowInputRosterService.ResolveGridShape(2));
        Assert.Equal((2, 2), ShowInputRosterService.ResolveGridShape(4));
        Assert.Equal((5, 2), ShowInputRosterService.ResolveGridShape(10));
    }

    [Fact]
    public void SyncZoomParticipantSlots_FillsFreeSlotsWithoutDisturbingAssignedOnes()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        // Slot 1 is an operator-assigned capture card; slot 2 an operator-assigned Zoom guest.
        slots[0].Kind = ShowInputKind.UvcWebcam;
        slots[0].CaptureDeviceId = "cam-uvc";
        slots[0].InShow = true;
        slots[1].Kind = ShowInputKind.ZoomParticipant;
        slots[1].ParticipantId = "p-host";
        slots[1].InShow = true;

        // Roster: the already-shown host plus two new guests.
        ShowInputRosterService.SyncZoomParticipantSlots(slots, ["p-host", "p-guest-a", "p-guest-b"], autoAssign: true);

        // Capture card untouched; host stays in its slot; new guests fill the next FREE slots.
        Assert.Equal(ShowInputKind.UvcWebcam, slots[0].Kind);
        Assert.Equal("cam-uvc", slots[0].CaptureDeviceId);
        Assert.Equal("p-host", slots[1].ParticipantId);
        Assert.Equal("p-guest-a", slots[2].ParticipantId);
        Assert.Equal(ShowInputKind.ZoomParticipant, slots[2].Kind);
        Assert.True(slots[2].InShow);
        Assert.Equal("p-guest-b", slots[3].ParticipantId);
    }

    [Fact]
    public void SyncZoomParticipantSlots_FreesLeftParticipantsAndKeepsOthersStable()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.ZoomParticipant;
        slots[0].ParticipantId = "p-1";
        slots[0].InShow = true;
        slots[1].Kind = ShowInputKind.ZoomParticipant;
        slots[1].ParticipantId = "p-2";
        slots[1].InShow = true;

        // p-1 leaves; p-2 stays; p-3 joins.
        ShowInputRosterService.SyncZoomParticipantSlots(slots, ["p-2", "p-3"], autoAssign: true);

        // p-2 keeps ITS slot (no reshuffle); p-1's slot is freed then reused for p-3.
        Assert.Equal("p-2", slots[1].ParticipantId);
        Assert.Equal("p-3", slots[0].ParticipantId);
        Assert.True(slots[0].InShow);
    }

    [Fact]
    public void SyncZoomParticipantSlots_WhenDisabledOnlyFreesLeftParticipants()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.ZoomParticipant;
        slots[0].ParticipantId = "p-gone";
        slots[0].InShow = true;

        ShowInputRosterService.SyncZoomParticipantSlots(slots, ["p-new"], autoAssign: false);

        // The departed participant is freed, but with auto-assign OFF nothing is auto-filled.
        Assert.Equal(ShowInputKind.Unassigned, slots[0].Kind);
        Assert.DoesNotContain(slots, s => s.ParticipantId == "p-new");
    }

    [Fact]
    public void SyncZoomParticipantSlots_StopsWhenEverySlotIsTaken()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        var roster = new List<string>();
        for (var i = 0; i < ShowInputRosterService.MaxShowInputs + 3; i++)
        {
            roster.Add($"p-{i}");
        }

        ShowInputRosterService.SyncZoomParticipantSlots(slots, roster, autoAssign: true);

        Assert.All(slots, slot => Assert.Equal(ShowInputKind.ZoomParticipant, slot.Kind));
        Assert.Equal(ShowInputRosterService.MaxShowInputs, slots.Count(s => s.IsAssigned));
    }

    [Fact]
    public void ResolveDisplayName_PrefersOverrideThenFallsBackToDerived()
    {
        var overrides = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["zoom:p-1"] = "  Dr. Jane Smith  ",
            ["capture:cam-uvc"] = "",  // blank override must be ignored
        };

        Assert.Equal("Dr. Jane Smith", ShowInputRosterService.ResolveDisplayName(overrides, "zoom:p-1", "Jane"));
        Assert.Equal("USB Capture", ShowInputRosterService.ResolveDisplayName(overrides, "capture:cam-uvc", "USB Capture"));
        Assert.Equal("Bumper", ShowInputRosterService.ResolveDisplayName(overrides, "media:asset-7", "Bumper"));
        Assert.Equal("Fallback", ShowInputRosterService.ResolveDisplayName(null, "zoom:p-1", "Fallback"));
    }

    [Fact]
    public void SlotSourceId_ReturnsCanonicalKeyPerKind()
    {
        Assert.Equal("zoom:p-1", ShowInputRosterService.SlotSourceId(
            new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.ZoomParticipant, ParticipantId = "p-1" }));
        Assert.Equal("capture:cam-uvc", ShowInputRosterService.SlotSourceId(
            new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "cam-uvc" }));
        Assert.Equal("media:asset-7", ShowInputRosterService.SlotSourceId(
            new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.Media, ParticipantId = ShowInputRosterService.ToMediaSourceId("asset-7") }));
        Assert.Null(ShowInputRosterService.SlotSourceId(new ShowInputSlot { SlotNumber = 1 }));
    }

    [Fact]
    public void BuildMultiviewLayoutSources_AppliesDisplayNameOverridesToLabels()
    {
        var slots = ShowInputRosterService.CreateDefaultSlots().ToList();
        slots[0].Kind = ShowInputKind.ZoomParticipant;
        slots[0].ParticipantId = "p-1";
        slots[0].InShow = true;
        slots[1].Kind = ShowInputKind.UvcWebcam;
        slots[1].CaptureDeviceId = "cam-uvc";
        slots[1].InShow = true;

        var participants = new[] { new Participant { Id = "p-1", Name = "Jane", Health = FeedHealth.Live } };
        var devices = new[] { Device("cam-uvc", "USB Capture", "uvc", 1920, 1080, 60, connected: true) };
        var overrides = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["zoom:p-1"] = "Dr. Jane Smith",
            ["capture:cam-uvc"] = "Main Camera",
        };

        var sources = ShowInputRosterService.BuildMultiviewLayoutSources(slots, participants, devices, null, overrides);

        Assert.Equal("Dr. Jane Smith", sources.Single(s => s.SourceId == "zoom:p-1").Label);
        Assert.Equal("Main Camera", sources.Single(s => s.SourceId == "capture:cam-uvc").Label);
    }

    private static MediaAsset Media(string id, string name, string kind, bool isPlaying = false) =>
        new()
        {
            Id = id,
            Name = name,
            Kind = kind,
            FilePath = $"C:\\media\\{id}.mp4",
            NaturalWidth = 1920,
            NaturalHeight = 1080,
            IsPlaying = isPlaying
        };

    private static CaptureDevice Device(
        string id,
        string name,
        string vendor,
        int width,
        int height,
        int frameRate,
        bool connected = false) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            Vendor = vendor,
            Inputs = [new CaptureDeviceInput { Id = "input-1", Label = "Input 1" }],
            SelectedInputId = "input-1",
            Width = width,
            Height = height,
            FrameRate = frameRate,
            ConnectionState = connected ? CaptureConnectionState.Connected : CaptureConnectionState.Detected,
            SignalPresent = connected
        };

    private static AudioCaptureDevice AudioDevice(
        string id,
        string name,
        string sourceKind,
        string driverName) =>
        new()
        {
            Id = id,
            NativeDeviceId = $"native-{id}",
            Name = name,
            SourceKind = sourceKind,
            DriverName = driverName
        };
}
