using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsSocialPlayoutTests
{
    private sealed class Fixture
    {
        public readonly List<(string Action, JsonElement Body)> Commands = [];
        public readonly TaskCompletionSource Preparing = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly TaskCompletionSource Ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public NativeGraphicsState State = new() { CoreEpoch = "epoch",
            Templates = [new() { TemplateId = GraphicsSocialPlayout.TemplateId, TemplateVersion = 1, Definition = GraphicsSocialPlayout.CardDefinition }] };
        public GraphicsSocialPlayout Create() => new(Invoke, () => State,
            async (_, ct) => { Preparing.TrySetResult(); await Ready.Task.WaitAsync(ct); });

        private Task<NativeGraphicsOperationResult> Invoke(string action, string request, CancellationToken cancellation)
        {
            var body = JsonSerializer.Deserialize<JsonElement>(request);
            Commands.Add((action, body));
            var id = body.GetProperty("instanceId").GetString()!;
            var current = State.Instances.FirstOrDefault();
            var next = (action, body.TryGetProperty("action", out var intent) ? intent.GetString() : "") switch
            {
                ("graphics.instance.create", _) => new NativeGraphicsInstance { InstanceId = id, Revision = 1 },
                ("graphics.instance.cue", _) => current! with { CuePending = true },
                ("graphics.instance.trigger", "show") => current! with { ProgramRevision = 1, CueRevision = 0, Visibility = "visible" },
                ("graphics.instance.trigger", "clear") => current! with { ProgramRevision = 0, CueRevision = 0 },
                ("graphics.instance.remove", _) => null,
                _ => throw new InvalidOperationException(action)
            };
            State = State with { Instances = next is null ? [] : [next] };
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, CurrentRevision = 1, InstanceId = id });
        }

        public void FinishPreparation()
        {
            State = State with { Instances = [State.Instances.Single() with { CueRevision = 1, CuePending = false }] };
            Ready.SetResult();
        }
    }

    [Fact]
    public async Task ApprovalAloneIsPrivateAndShowUsesOnlyTheSelectedExactRevision()
    {
        var fixture = new Fixture();
        var playout = fixture.Create();
        var source = await playout.ConnectAsync();
        var approved = (await playout.ReceiveAsync(source, "a", "Host", "First message"))!;
        var pending = (await playout.ReceiveAsync(source, "b", "Guest", "Second message"))!;
        Assert.True(playout.Approve(approved.Key));
        Assert.False((await playout.ShowAsync(pending.Key)).Accepted);
        Assert.Empty(fixture.Commands);
        var show = playout.ShowAsync(approved.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var create = fixture.Commands[0].Body;
        Assert.Equal("First message", create.GetProperty("fields").GetProperty("message").GetString());
        Assert.False(fixture.Commands[1].Body.GetProperty("rideTake").GetBoolean());
        Assert.Empty(fixture.State.Instances.Where(i => i.ProgramRevision > 0));
        fixture.FinishPreparation();
        Assert.True((await show).Accepted);
        Assert.True(playout.OnProgram);
        await playout.WithdrawAsync(source, "a");
        Assert.False(playout.OnProgram);
        Assert.Empty(fixture.State.Instances);
    }

    [Theory]
    [InlineData("edit")]
    [InlineData("remove")]
    [InlineData("disconnect")]
    public async Task InvalidationWhilePreparingNeverSubmitsShow(string invalidation)
    {
        var fixture = new Fixture();
        var playout = fixture.Create();
        var source = await playout.ConnectAsync();
        var item = (await playout.ReceiveAsync(source, "a", "Host", "Original"))!;
        Assert.True(playout.Approve(item.Key));
        var show = playout.ShowAsync(item.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Task invalidated = invalidation switch {
            "edit" => playout.ReceiveAsync(source, "a", "Host", "Changed"),
            "remove" => playout.WithdrawAsync(source, "a"),
            _ => playout.DisconnectAsync()
        };
        fixture.FinishPreparation();
        Assert.False((await show).Accepted);
        await invalidated;
        Assert.DoesNotContain(fixture.Commands, c => c.Action == "graphics.instance.trigger" &&
            c.Body.GetProperty("action").GetString() == "show");
        Assert.Empty(fixture.State.Instances);
    }
}
