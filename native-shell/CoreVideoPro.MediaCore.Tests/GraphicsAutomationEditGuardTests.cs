using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsAutomationEditGuardTests
{
    private static NativeGraphicsAutomation State => new() { Mode = "automatic", Configuration = new() {
        Rules = [new() { Id = "intro", InstanceId = "cue", SourceId = "guest-a" }] } };

    [Fact] public void InitialAndExternalModeChangesLoadWithoutAnOperatorEdit()
    {
        var guard = new GraphicsAutomationEditGuard();
        Assert.True(guard.Observe("epoch", "cue", State));
        Assert.False(guard.Observe("epoch", "cue", State));
        Assert.True(guard.Observe("epoch", "cue", State with { Mode = "rehearsal" }));
    }

    [Fact] public void TraceArmingAndUnrelatedSourceTextRevisionDoNotErasePendingFields()
    {
        var guard = new GraphicsAutomationEditGuard(); guard.Observe("epoch", "cue", State); guard.MarkEdited();
        Assert.False(guard.Observe("epoch", "cue", State with { Revision = 20, Armed = true,
            Trace = [new() { Reason = "subject-dwell" }], Configuration = State.Configuration with {
                SourceTexts = new Dictionary<string, GraphicsAutomationSourceText> { ["guest-b"] = new() { Name = "Updated" } } } }));
        Assert.True(guard.Dirty); Assert.False(guard.Conflict);
    }

    [Fact] public void ConcurrentRuleChangeRetainsDraftAndRequiresExplicitReload()
    {
        var guard = new GraphicsAutomationEditGuard(); guard.Observe("epoch", "cue", State); guard.MarkEdited();
        var changed = State with { Configuration = new() { Rules = [State.Configuration.Rules[0] with { SourceId = "guest-b" }] } };
        Assert.False(guard.Observe("epoch", "cue", changed)); Assert.True(guard.Conflict);
        guard.Reload(); Assert.True(guard.Observe("epoch", "cue", changed));
        Assert.False(guard.Dirty); Assert.False(guard.Conflict);
    }

    [Theory]
    [InlineData("new-epoch", "cue", "automatic")]
    [InlineData("epoch", "other-cue", "automatic")]
    [InlineData("epoch", "cue", "manual")]
    public void ReconnectTargetOrModeChangeCannotSilentlyReplacePendingEdits(string epoch, string target, string mode)
    {
        var guard = new GraphicsAutomationEditGuard(); guard.Observe("epoch", "cue", State); guard.MarkEdited();
        Assert.False(guard.Observe(epoch, target, State with { Mode = mode })); Assert.True(guard.Conflict);
    }

    [Theory]
    [InlineData("manual", "No rule")]
    [InlineData("suggest", "never reveals")]
    [InlineData("rehearsal", "Program stays unchanged")]
    [InlineData("automatic", "after Arm")]
    public void ModeGuidanceNamesTheActualProgramBoundary(string mode, string expected) =>
        Assert.Contains(expected, GraphicsAutomationEditGuard.ModeHelp(mode));
}
