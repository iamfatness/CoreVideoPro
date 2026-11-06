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
            Templates = [new() { TemplateId = GraphicsSocialPlayout.TemplateId, TemplateVersion = 1, Runtime = "declarative",
                Definition = GraphicsSocialPlayout.CardDefinition, Fields = [
                    new() { Id = "author", Type = "string", MaxLength = 80, Required = true },
                    new() { Id = "message", Type = "string", MaxLength = 192, Required = true }] }] };
        public GraphicsSocialPlayout Create() => new(Invoke, () => State,
            async (_, ct) => { Preparing.TrySetResult(); await Ready.Task.WaitAsync(ct); });

        private Task<NativeGraphicsOperationResult> Invoke(string action, string request, CancellationToken cancellation)
        {
            Assert.True(GraphicsCommandRequest.TryBuild(action, request, State, out _, out _, out var error), error);
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

    [Theory]
    [InlineData("YouTube")]
    [InlineData("Twitch")]
    public async Task ApprovalAloneIsPrivateAndShowUsesOnlyTheSelectedExactRevision(string provider)
    {
        var fixture = new Fixture();
        if (provider == "Twitch") fixture.State = fixture.State with {
            Templates = [fixture.State.Templates[0] with { TemplateId = "twitch-chat-card", Definition = GraphicsSocialPlayout.TwitchCardDefinition }] };
        var playout = fixture.Create();
        var source = await playout.ConnectAsync(provider);
        var approved = (await playout.ReceiveAsync(source, "a", "Host", "First message"))!;
        var pending = (await playout.ReceiveAsync(source, "b", "Guest", "Second message"))!;
        Assert.Equal(provider, approved.Provider);
        Assert.True(playout.Approve(approved.Key));
        Assert.False((await playout.ShowAsync(pending.Key)).Accepted);
        Assert.Empty(fixture.Commands);
        var show = playout.ShowAsync(approved.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var create = fixture.Commands[0].Body;
        Assert.Equal(provider == "Twitch" ? "twitch-chat-card" : "youtube-chat-card", create.GetProperty("templateId").GetString());
        Assert.Equal("First message", create.GetProperty("fields").GetProperty("message").GetString());
        Assert.False(fixture.Commands[1].Body.GetProperty("rideTake").GetBoolean());
        Assert.DoesNotContain(fixture.State.Instances, i => i.ProgramRevision > 0);
        fixture.FinishPreparation();
        Assert.True((await show).Accepted);
        Assert.True(playout.OnProgram);
        await playout.WithdrawAsync(source, "a");
        Assert.False(playout.OnProgram);
        Assert.Empty(fixture.State.Instances);
    }

    [Fact]
    public async Task ProviderReplacementCannotReuseApprovalOrAcceptOldCallbacks()
    {
        var fixture = new Fixture();
        var playout = fixture.Create();
        var source = await playout.ConnectAsync("Twitch");
        var item = (await playout.ReceiveAsync(source, "1234:a", "Host", "Reviewed Twitch message"))!;
        Assert.True(playout.Approve(item.Key));
        await playout.ConnectAsync("YouTube");
        Assert.Null(await playout.ReceiveAsync(source, "1234:b", "Host", "Late Twitch callback"));
        Assert.False((await playout.ShowAsync(item.Key)).Accepted);
        Assert.Empty(fixture.Commands);
        Assert.Empty(playout.Items);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task TwitchClearInvalidatesPreparationOrRetiresProgramAndFencesLateMessages(bool room, bool onAir)
    {
        var fixture = new Fixture();
        fixture.State = fixture.State with { Templates = [fixture.State.Templates[0] with {
            TemplateId = "twitch-chat-card", Definition = GraphicsSocialPlayout.TwitchCardDefinition }] };
        var playout = fixture.Create();
        var source = await playout.ConnectAsync("Twitch");
        var message = new SocialFeedEvent(SocialFeedEventKind.Text, "1234:a", "5678", "Host", "Reviewed message", 100);
        await playout.ApplyAsync(source, [message]);
        var approved = Assert.Single(playout.Items);
        Assert.True(playout.Approve(approved.Key));
        var show = playout.ShowAsync(approved.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        if (onAir)
        {
            fixture.FinishPreparation();
            Assert.True((await show).Accepted);
            Assert.True(playout.OnProgram);
        }
        var clear = playout.ApplyAsync(source, [new(room ? SocialFeedEventKind.ClearRoom : SocialFeedEventKind.ClearUser,
            AuthorId: "5678", Order: 200)]);
        if (!onAir)
        {
            fixture.FinishPreparation();
            Assert.False((await show).Accepted);
            Assert.DoesNotContain(fixture.Commands, c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "show");
        }
        await clear;
        Assert.False(playout.OnProgram);
        Assert.Empty(fixture.State.Instances);
        Assert.False(playout.Approve(approved.Key));
        await playout.ApplyAsync(source, [message, message with { Id = "1234:late", Order = 199 }]);
        Assert.Empty(playout.Items);
        await playout.ApplyAsync(source, [message with { Id = "1234:new", Order = 201 }]);
        Assert.Equal(GraphicsSocialDecision.Pending, Assert.Single(playout.Items).Decision);
    }

    [Theory]
    [InlineData("edit")]
    [InlineData("remove")]
    [InlineData("disconnect")]
    [InlineData("provider-ban")]
    public async Task InvalidationWhilePreparingNeverSubmitsShow(string invalidation)
    {
        var fixture = new Fixture();
        var playout = fixture.Create();
        var source = await playout.ConnectAsync();
        Assert.True(await playout.ApplyAsync(source, [new(SocialFeedEventKind.Text, "a", "host-id", "Host", "Original")]));
        var item = Assert.Single(playout.Items);
        Assert.True(playout.Approve(item.Key));
        var show = playout.ShowAsync(item.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Task invalidated = invalidation switch {
            "edit" => playout.ReceiveAsync(source, "a", "Host", "Changed"),
            "remove" => playout.WithdrawAsync(source, "a"),
            "provider-ban" => playout.ApplyAsync(source, [new(SocialFeedEventKind.Ban, AuthorId: "host-id")]),
            _ => playout.DisconnectAsync()
        };
        fixture.FinishPreparation();
        Assert.False((await show).Accepted);
        await invalidated;
        Assert.DoesNotContain(fixture.Commands, c => c.Action == "graphics.instance.trigger" &&
            c.Body.GetProperty("action").GetString() == "show");
        Assert.Empty(fixture.State.Instances);
    }
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ProviderModerationRetiresOnAirCardAndRejectsSameBatchReplay(bool banAuthor)
    {
        var kind = banAuthor ? SocialFeedEventKind.Ban : SocialFeedEventKind.Remove;
        var fixture = new Fixture();
        var playout = fixture.Create();
        var source = await playout.ConnectAsync();
        var reviewed = new SocialFeedEvent(SocialFeedEventKind.Text, "a", "host-id", "Host", "Reviewed message");
        var other = new SocialFeedEvent(SocialFeedEventKind.Text, "b", "guest-id", "Guest", "Other message");
        Assert.True(await playout.ApplyAsync(source, [reviewed, other]));
        var card = playout.Items.Single(item => item.Key.Id == "a");
        var untouched = playout.Items.Single(item => item.Key.Id == "b");
        Assert.True(playout.Approve(card.Key));
        Assert.True(playout.Approve(untouched.Key));
        var show = playout.ShowAsync(card.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        fixture.FinishPreparation();
        Assert.True((await show).Accepted);
        Assert.True(playout.OnProgram);

        var moderation = new SocialFeedEvent(kind, Id: "a", AuthorId: "host-id");
        var otherReplay = reviewed with { Id = "new-id" };
        Assert.True(await playout.ApplyAsync(source, [moderation, reviewed, otherReplay]));
        Assert.False(playout.OnProgram);
        Assert.Empty(fixture.State.Instances);
        Assert.DoesNotContain(playout.Items, item => item.Key.Id == "a");
        Assert.Equal(!banAuthor, playout.Items.Any(item => item.Key.Id == "new-id"));
        Assert.Equal(GraphicsSocialDecision.Approved, playout.Items.Single(item => item.Key.Id == "b").Decision);
        var commands = fixture.Commands.Count;
        Assert.False((await playout.ShowAsync(card.Key)).Accepted);
        Assert.Equal(commands, fixture.Commands.Count);
    }

    [Fact]
    public async Task ReplacementDuringPreparationRejectsOldCallbacksAndStartsWithPendingReview()
    {
        var fixture = new Fixture();
        var playout = fixture.Create();
        var firstSource = await playout.ConnectAsync();
        var first = (await playout.ReceiveAsync(firstSource, "id", "First host", "First message"))!;
        Assert.True(playout.Approve(first.Key));
        var show = playout.ShowAsync(first.Key);
        await fixture.Preparing.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var replacement = playout.ConnectAsync();
        fixture.FinishPreparation();
        Assert.False((await show).Accepted);
        var secondSource = await replacement;
        var second = (await playout.ReceiveAsync(secondSource, "id", "Second host", "Second message"))!;

        Assert.False(await playout.ApplyAsync(firstSource, [
            new(SocialFeedEventKind.Remove, Id: "id"),
            new(SocialFeedEventKind.Text, "ghost", "old-id", "Old host", "Late delivery")]));
        await playout.DisconnectAsync(firstSource);
        Assert.True(playout.IsConnected);
        Assert.Equal(second, Assert.Single(playout.Items));
        Assert.Equal(GraphicsSocialDecision.Pending, second.Decision);
        Assert.False(playout.Approve(first.Key));
        Assert.False((await playout.ShowAsync(second.Key)).Accepted);
        Assert.DoesNotContain(fixture.Commands, command => command.Action == "graphics.instance.trigger" &&
            command.Body.GetProperty("action").GetString() == "show");
    }

}
