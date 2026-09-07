using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>The per-route rewrite behind an OHG look (spec §8 applyLook / D11 route naming).
///
/// The case that matters most is an UNASSIGNED slot: publish-time resolution
/// (<c>ResolveRouteFromShowInput</c> → <c>ShowInputRosterService.ApplySlotRoute</c>) returns
/// early on one, so if the writer does not clear the route's own source ids the box composites
/// WHOEVER IT CARRIED BEFORE — on preview, then on PROGRAM after the take.</summary>
public sealed class OhgRouteSlotWriterTests
{
    private static SourceRoute RouteCarrying(string? participantId = "guest-was-here", string? captureDeviceId = null)
        => new()
        {
            Id = "ohg-box-1",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = participantId,
            CaptureDeviceId = captureDeviceId,
            ShowInputSlotNumber = 4,
            ProductionRoleId = "role-host",
            SpotlightIndex = 2
        };

    private static ShowInputSlot Slot(int number, ShowInputKind kind = ShowInputKind.Unassigned,
        string? participantId = null, string? captureDeviceId = null)
        => new()
        {
            SlotNumber = number,
            Kind = kind,
            ParticipantId = participantId,
            CaptureDeviceId = captureDeviceId
        };

    [Fact]
    public void AnUnassignedSlotClearsTheRoutesPreviousSource()
    {
        var route = RouteCarrying();

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 3, Slot(3));

        Assert.Equal(3, route.ShowInputSlotNumber);
        Assert.Null(route.ParticipantId);
        Assert.Null(route.CaptureDeviceId);
        // Fixed with no participant = the placeholder slate (an empty box). NOT None, which
        // would drop the layer out of the render plan entirely.
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
    }

    [Fact]
    public void AnUnassignedSlotAlsoClearsAPreviousCaptureDevice()
    {
        var route = RouteCarrying(participantId: null, captureDeviceId: "cam-1");

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 3, Slot(3));

        Assert.Null(route.CaptureDeviceId);
    }

    [Fact]
    public void AMissingSlotIsTreatedLikeAnUnassignedOne()
    {
        var route = RouteCarrying();

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 9, resolvedSlot: null);

        Assert.Equal(9, route.ShowInputSlotNumber);
        Assert.Null(route.ParticipantId);
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
    }

    [Fact]
    public void AnAssignedZoomSlotCarriesItsParticipant()
    {
        var route = RouteCarrying(participantId: "stale-guest");

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 2, Slot(2, ShowInputKind.ZoomParticipant, participantId: "16778240"));

        Assert.Equal(2, route.ShowInputSlotNumber);
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("16778240", route.ParticipantId);
        Assert.Null(route.CaptureDeviceId);
    }

    [Fact]
    public void AnAssignedCaptureSlotBecomesACaptureDeviceRoute()
    {
        var route = RouteCarrying(participantId: "stale-guest");

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 5, Slot(5, ShowInputKind.UvcWebcam, captureDeviceId: "cam-42"));

        Assert.Equal(SourceRouteMode.CaptureDevice, route.Mode);
        Assert.Equal("cam-42", route.CaptureDeviceId);
        Assert.Null(route.ParticipantId);
    }

    [Fact]
    public void ANullSlotEmptiesTheRouteCompletely()
    {
        var route = RouteCarrying();

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, null, Slot(4, ShowInputKind.ZoomParticipant, participantId: "x"));

        Assert.Equal(SourceRouteMode.None, route.Mode);
        Assert.Null(route.ShowInputSlotNumber);
        Assert.Null(route.ParticipantId);
        Assert.Null(route.CaptureDeviceId);
    }

    [Fact]
    public void EveryWriteClearsARoleTargetAndSpotlight()
    {
        // R1: ResolveRouteFromShowInput short-circuits on a non-empty ProductionRoleId and never
        // looks at the slot, so a surviving role id would make the whole look a no-op.
        var route = RouteCarrying();

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 1, Slot(1, ShowInputKind.ZoomParticipant, participantId: "p1"));

        Assert.Null(route.ProductionRoleId);
        Assert.Null(route.SpotlightIndex);
    }

    [Fact]
    public void AMismatchedResolvedSlotIsNotTrusted()
    {
        // Defensive: the caller looks the slot up by number; a slot carrying a DIFFERENT number
        // must never donate its source to this route.
        var route = RouteCarrying();

        OhgRouteSlotWriter.ApplyOhgSlotToRoute(route, 3, Slot(7, ShowInputKind.ZoomParticipant, participantId: "wrong"));

        Assert.Equal(3, route.ShowInputSlotNumber);
        Assert.Null(route.ParticipantId);
    }
}
