using System.Linq;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// Scenes redesign R1: role-targeted routes. The role is a stable TARGET stored
// on the route; the participant holding it is resolved at sync time, so saved
// scenes stay valid no matter who joins.
public class ProductionRoleTests
{
    [Fact]
    public void RoleOptionValuesRoundTripThroughTheParser()
    {
        foreach (var role in ProductionRoleService.Roles)
        {
            var optionValue = ProductionRoleService.ToOptionValue(role.Value);
            Assert.True(ProductionRoleService.IsRoleOption(optionValue));
            Assert.Equal(role.Value, ProductionRoleService.RoleIdFromOption(optionValue));
        }

        Assert.False(ProductionRoleService.IsRoleOption("participant-1"));
        Assert.False(ProductionRoleService.IsRoleOption("input-01"));
        Assert.Null(ProductionRoleService.RoleIdFromOption("media:abc"));
    }

    [Fact]
    public void AddingARoleSourceBuildsAFixedRouteCarryingTheRoleAndNoParticipant()
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 0, "role:reader");

        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("reader", route.ProductionRoleId);
        Assert.Null(route.ParticipantId);
        Assert.Null(route.ShowInputSlotNumber);
    }

    [Fact]
    public void AddSourceOptionsIncludeEveryRole()
    {
        foreach (var role in ProductionRoleService.Roles)
        {
            Assert.Contains(SceneRoutingService.AddSourceOptions, option =>
                option.Value == ProductionRoleService.ToOptionValue(role.Value) &&
                option.Label == $"Role: {role.Label}");
        }
    }

    [Fact]
    public void ProductionRoleSurvivesScenePersistenceRoundTrip()
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 0, "role:host");
        route.CanvasRect = new NormalizedCanvasRect { X = 0.1, Y = 0.1, Width = 0.5, Height = 0.5 };

        var persisted = ScenePersistenceService.ToPersisted(
            new Scene { Id = "scene-1", Name = "Roles", Layout = "custom" },
            [route]);
        var restoredRoute = ScenePersistenceService.FromPersisted(Assert.Single(persisted.Routes));

        Assert.Equal("host", restoredRoute.ProductionRoleId);
        Assert.Null(restoredRoute.ParticipantId);
    }

    [Fact]
    public void CloneCopiesTheProductionRole()
    {
        var route = SceneRoutingService.BuildAddedSourceRoute("scene-1", 0, "role:guest-2");
        Assert.Equal("guest-2", route.Clone().ProductionRoleId);
    }

    [Fact]
    public void FeedHealthCarriesPerParticipantZoomSubscriptionEvidence()
    {
        var rows = ProductionStateHelper.BuildFeedHealthRows(
            [new Participant { Id = "42", Name = "Guest", Health = FeedHealth.Live }],
            subscriptions:
            [
                new ZoomMediaSpineSubscription
                {
                    ParticipantId = "42",
                    Kind = "participant-video",
                    Status = "subscribed",
                    LastResultCode = "ok",
                    DeliveredWidth = 1280,
                    DeliveredHeight = 720,
                    DeliveredFps = 30,
                    FramesReceived = 900,
                    FrameFresh = true
                },
                new ZoomMediaSpineSubscription
                {
                    ParticipantId = "42",
                    Kind = "participant-audio",
                    Status = "subscribed",
                    AudioPacketsReceived = 1500
                }
            ]);

        var row = Assert.Single(rows);
        Assert.Equal("1280x720 @ 30fps", row.DeliveredVideo);
        Assert.Equal(900, row.VideoFramesReceived);
        Assert.Equal(1500, row.AudioPacketsReceived);
        Assert.Contains("Video subscribed", row.DiagnosticSummary);
        Assert.False(row.HasRecommendedAction);
    }

    [Fact]
    public void FeedHealthCarriesThePerSourceDropoutPolicy()
    {
        // #535 slice 4a: the operator's persisted "on dropout" choice reads
        // through onto the matching row; every other row defaults to "hold".
        var rows = ProductionStateHelper.BuildFeedHealthRows(
            [
                new Participant { Id = "1", Name = "Black guest", Health = FeedHealth.Live },
                new Participant { Id = "2", Name = "Default guest", Health = FeedHealth.Live }
            ],
            dropoutPolicies: new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["zoom:1"] = "black"
            });

        var blackRow = rows.Single(row => row.ParticipantId == "1");
        var defaultRow = rows.Single(row => row.ParticipantId == "2");

        Assert.Equal("black", blackRow.DropoutPolicy);
        Assert.Equal("hold", defaultRow.DropoutPolicy);
    }

    [Fact]
    public void ResolveSourceDropoutPolicyReadsBackACaptureDevicesStoredPolicy()
    {
        // #535 slice 4a fix round 1: capture-device rows share the exact same
        // resolution helper as Zoom guest rows (ProductionStateHelper.
        // ResolveSourceDropoutPolicy), so a capture row never shows a stale
        // "Hold last frame" default after a relaunch when "black" was stored.
        var dropoutPolicies = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["capture:cam-1"] = "black"
        };

        Assert.Equal("black", ProductionStateHelper.ResolveSourceDropoutPolicy("capture:cam-1", dropoutPolicies));
        Assert.Equal("hold", ProductionStateHelper.ResolveSourceDropoutPolicy("capture:cam-2", dropoutPolicies));
    }
}
