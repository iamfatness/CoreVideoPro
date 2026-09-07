using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>The two OHG decisions <see cref="StudioControlSurface"/> makes, extracted as pure
/// statics so they are testable without a <c>DispatcherQueue</c> (constructing the surface needs
/// a UI thread; every existing coverage test avoids it for the same reason).
///
/// 1. <see cref="StudioControlSurface.IsBridgeAction"/> — which ids skip the UI-thread marshal
///    and go straight to the show engine (spec §3: "ohg.* invokes never touch the UI thread").
/// 2. <see cref="StudioControlSurface.WithOhg"/> — the §7 state-node projection.</summary>
public sealed class StudioControlSurfaceOhgForwardingTests
{
    // ---- IsBridgeAction --------------------------------------------------------------

    [Theory]
    [InlineData("ohg.panelist.add")]
    [InlineData("ohg.program.cut")]
    [InlineData("ohg.a")]
    public void EngineActionIdsAreBridgeActions(string id)
        => Assert.True(StudioControlSurface.IsBridgeAction(id));

    [Theory]
    [InlineData("ohgx.y")]        // the prefix must include the DOT: "ohgx" is not the engine
    [InlineData("ohg")]
    [InlineData("ohgboard.set")]
    [InlineData("transport.take")]
    [InlineData("showEngine.restart")]  // a SHELL action, handled by the surface, not forwarded
    [InlineData("Ohg.panelist.add")]    // ordinal, case-sensitive
    [InlineData("")]
    public void EverythingElseIsNotABridgeAction(string id)
        => Assert.False(StudioControlSurface.IsBridgeAction(id));

    // ---- WithOhg ---------------------------------------------------------------------

    private static ShowEngineHealth Health(ShowEngineState state)
        => new(state, Generation: 3, RestartCount: 1, LastError: null, LastCrashAt: null);

    private static ShowEngineSnapshot Snapshot()
    {
        using var snapshotDoc = JsonDocument.Parse("""{"revision":7}""");
        using var fieldDoc = JsonDocument.Parse("""{"v":"live"}""");
        return new ShowEngineSnapshot(
            Generation: 3,
            Revision: 7,
            Snapshot: snapshotDoc.RootElement.Clone(),
            Fields: new Dictionary<string, JsonElement>
            {
                ["ohg/slot/1/name"] = fieldDoc.RootElement.GetProperty("v").Clone()
            });
    }

    [Fact]
    public void NoSnapshotLeavesTheOhgNodeNull()
    {
        var state = StudioControlSurface.WithOhg(ControlState.Empty, latest: null, Health(ShowEngineState.Starting), shadowLast: null);

        Assert.Null(state.Ohg);
        Assert.Null(state.OhgFields);
        Assert.Equal("starting", state.OhgEngineHealth);
        Assert.Equal(string.Empty, state.OhgShadowLastCommand);
    }

    [Theory]
    [InlineData(ShowEngineState.Stopped, "stopped")]
    [InlineData(ShowEngineState.Starting, "starting")]
    [InlineData(ShowEngineState.Running, "running")]
    [InlineData(ShowEngineState.Recovering, "recovering")]
    [InlineData(ShowEngineState.Failed, "failed")]
    public void EveryEngineStateProjectsItsLowercaseName(ShowEngineState state, string expected)
    {
        Assert.Equal(expected, StudioControlSurface.WithOhg(ControlState.Empty, null, Health(state), null).OhgEngineHealth);
    }

    [Fact]
    public void ASnapshotProjectsItsRawNodeAndFlatFields()
    {
        var state = StudioControlSurface.WithOhg(ControlState.Empty, Snapshot(), Health(ShowEngineState.Running), null);

        Assert.NotNull(state.Ohg);
        Assert.Equal(7, state.Ohg!.Value.GetProperty("revision").GetInt32());
        Assert.Equal("live", Assert.Contains("ohg/slot/1/name", state.OhgFields!).GetString());
        Assert.Equal("running", state.OhgEngineHealth);
    }

    [Fact]
    public void TheShadowLastCommandIsCarried()
    {
        var state = StudioControlSurface.WithOhg(ControlState.Empty, null, Health(ShowEngineState.Running), "12 cut([])");

        Assert.Equal("12 cut([])", state.OhgShadowLastCommand);
    }

    [Fact]
    public void ANullShadowLastCommandBecomesTheEmptyString()
    {
        // ControlState.OhgShadowLastCommand is non-nullable and always serialized; "no command
        // yet" must be the empty string, never a null that breaks a Companion variable.
        Assert.Equal(string.Empty, StudioControlSurface.WithOhg(ControlState.Empty, null, Health(ShowEngineState.Running), null).OhgShadowLastCommand);
    }

    [Fact]
    public void TheRestOfTheBaseStateSurvivesUntouched()
    {
        var baseState = ControlState.Empty with { CommandStatus = "Take armed", Recording = true };

        var state = StudioControlSurface.WithOhg(baseState, Snapshot(), Health(ShowEngineState.Running), "1 cut([])");

        Assert.Equal("Take armed", state.CommandStatus);
        Assert.True(state.Recording);
    }

    [Fact]
    public void TheShellRestartActionIsCoveredByTheSurface()
        => Assert.Contains("showEngine.restart", StudioControlSurface.SupportedActionIds);
}
