using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// #762: the shell used to accept any core. These pin the three refusals and the cases
// that must stay open.
public sealed class MediaCoreOnAirPolicyTests
{
    private static NativeMediaCoreProfile Profile(
        string renderer = "d3d11",
        params (string Capability, string State, string Detail)[] states) =>
        new()
        {
            Name = renderer == "software" ? "CoreVideo Pro Native Media Core Stub" : "CoreVideo Pro Native Media Core",
            Renderer = renderer,
            MaxProgramResolution = "3840x2160",
            CapabilityStates = states.ToDictionary(
                state => state.Capability,
                state => new NativeMediaCoreCapabilityState { State = state.State, Detail = state.Detail })
        };

    private static readonly (string, string, string)[] Healthy =
    [
        ("gpu-compositor", "available", ""),
        ("program-recording", "available", ""),
        ("zoom-raw-video", "available", "")
    ];

    [Fact]
    public void AHealthyCoreIsNotBlocked()
    {
        var profile = Profile(states: Healthy);
        Assert.Null(MediaCoreOnAirPolicy.EngineBlockReason(profile));
        Assert.Null(MediaCoreOnAirPolicy.RecordBlockReason(profile));
        Assert.Null(MediaCoreOnAirPolicy.JoinBlockReason(profile));
    }

    [Fact]
    public void NoProfileYetIsNotARefusalHere()
    {
        Assert.Null(MediaCoreOnAirPolicy.EngineBlockReason(null));
        Assert.Null(MediaCoreOnAirPolicy.RecordBlockReason(null));
        Assert.Null(MediaCoreOnAirPolicy.JoinBlockReason(null));
    }

    [Fact]
    public void ASoftwareCompositorBlocksEngineAndRecordWithTheCoresDetail()
    {
        var profile = Profile("software",
            ("gpu-compositor", "failed-to-construct", ""),
            ("program-recording", "available", ""),
            ("zoom-raw-video", "available", ""));

        var reason = MediaCoreOnAirPolicy.EngineBlockReason(profile);
        Assert.NotNull(reason);
        Assert.Contains("no GPU compositor", reason);
        Assert.Contains("failed-to-construct", reason);
        Assert.Equal(reason, MediaCoreOnAirPolicy.RecordBlockReason(profile));
        Assert.Null(MediaCoreOnAirPolicy.JoinBlockReason(profile));
    }

    [Fact]
    public void AnEncoderThatDidNotConstructBlocksRecordOnly()
    {
        var profile = Profile(states:
        [
            ("gpu-compositor", "available", ""),
            ("program-recording", "failed-to-construct", "encoder-adapter-did-not-start"),
            ("zoom-raw-video", "available", "")
        ]);

        Assert.Null(MediaCoreOnAirPolicy.EngineBlockReason(profile));
        var reason = MediaCoreOnAirPolicy.RecordBlockReason(profile);
        Assert.NotNull(reason);
        Assert.Contains("no recording encoder", reason);
        Assert.Contains("encoder-adapter-did-not-start", reason);
        Assert.Contains("write no file", reason);
    }

    [Fact]
    public void AnOmittedRecordingCapabilityIsNotTreatedAsAFailedEncoder()
    {
        // "omitted" is the stub tier's word for "never built"; the stub tier records on purpose.
        var profile = Profile(states:
        [
            ("gpu-compositor", "available", ""),
            ("program-recording", "omitted", "stub-build"),
            ("zoom-raw-video", "available", "")
        ]);

        Assert.Null(MediaCoreOnAirPolicy.RecordBlockReason(profile));
    }

    [Fact]
    public void ACoreWithNoZoomEngineBlocksJoin()
    {
        var profile = Profile(states:
        [
            ("gpu-compositor", "available", ""),
            ("program-recording", "available", ""),
            ("zoom-raw-video", "omitted", "engine-path-not-configured")
        ]);

        var reason = MediaCoreOnAirPolicy.JoinBlockReason(profile);
        Assert.NotNull(reason);
        Assert.Contains("no Zoom engine", reason);
        Assert.Contains("engine-path-not-configured", reason);
        Assert.Null(MediaCoreOnAirPolicy.EngineBlockReason(profile));
    }

    [Fact]
    public void AnOlderCoreWithoutCapabilityStatesDoesNotBlockJoin()
    {
        var profile = new NativeMediaCoreProfile
        {
            Name = "CoreVideo Pro Native Media Core",
            Renderer = "d3d11",
            MaxProgramResolution = "3840x2160",
            Capabilities = ["gpu-compositor", "program-recording"]
        };

        Assert.Null(MediaCoreOnAirPolicy.JoinBlockReason(profile));
    }
}
