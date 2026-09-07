using System.Threading.Tasks;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 6 — the GFX/data panel commands and lamps. Reuses the rig from
/// <see cref="OhgShowViewModelTests"/> (<c>NewVm</c>, <c>Snapshot</c>).</summary>
public sealed partial class OhgShowViewModelTests
{
    /// <summary>A snapshot carrying a full <c>overlays</c>/<c>capabilities</c>/<c>health</c> node
    /// so the gfx panel has something real to project — the shared <c>SnapshotA</c>/<c>SnapshotB</c>
    /// fixtures omit these nodes entirely (they fall back to neutral defaults).</summary>
    private static string GfxSnapshot(
        string revision = "10",
        string headlineName = "Jane Doe",
        string headlineLocation = "Helsinki",
        bool headlineVisible = true,
        string registryState = "unavailable",
        string? registryDetail = "no registry configured",
        string handsState = "available",
        string questionState = "disabled",
        string worst = "failing")
    {
        var headlineVisibleJson = headlineVisible ? "true" : "false";
        var registryDetailJson = registryDetail is null ? "" : $",\"detail\":\"{registryDetail}\"";
        return "{"
            + $"\"revision\":{revision},"
            + "\"slots\":[],"
            + "\"tally\":{\"onAirSlots\":[]},"
            + "\"panelists\":[],"
            + "\"unseated\":[],"
            + "\"gallery\":[],"
            + "\"queue\":{\"previous\":[],\"current\":null,\"upcoming\":[]},"
            + "\"program\":{\"program\":{\"kind\":\"black\"},\"preview\":{\"kind\":\"black\"},\"activeSpeakerFollow\":false},"
            + "\"smartGallery\":false,"
            + "\"pagingRefused\":null,"
            + "\"restoreWarnings\":[],"
            + "\"overlays\":{"
            + "\"question\":{\"text\":\"What is your name?\",\"askerName\":\"Bob\"},"
            + $"\"headline\":{{\"name\":\"{headlineName}\",\"location\":\"{headlineLocation}\"}},"
            + $"\"headlineVisible\":{headlineVisibleJson}"
            + "},"
            + "\"capabilities\":{"
            + $"\"registry\":{{\"state\":\"{registryState}\"{registryDetailJson}}},"
            + $"\"handsQueue\":{{\"state\":\"{handsState}\"}},"
            + $"\"questionFeed\":{{\"state\":\"{questionState}\"}}"
            + "},"
            + $"\"health\":{{\"panelists\":{{\"state\":\"{worst}\"}},\"hands\":{{\"state\":\"ok\"}},\"question\":{{\"state\":\"ok\"}}}}"
            + "}";
    }

    // ── headline editor ────────────────────────────────────────────────────────────────

    [Fact]
    public async Task HeadlineIn_SendsHeadlineIn()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.HeadlineInCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gfx.headline.in", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task HeadlineOut_SendsHeadlineOut()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.HeadlineOutCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gfx.headline.out", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task HeadlineChange_SendsHeadlineChange_WithBothFields()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.HeadlineName = "Jane";
        vm.HeadlineLocation = "Helsinki";

        await vm.HeadlineChangeCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gfx.headline.change", actionId);
        Assert.Equal(new object?[] { "Jane", "Helsinki" }, args);
    }

    [Theory]
    [InlineData("", "Helsinki")]
    [InlineData("Jane", "")]
    [InlineData("", "")]
    [InlineData(null, "Helsinki")]
    public async Task HeadlineChange_MissingField_RefusesLocally_NeverInvokes(string? name, string? location)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.HeadlineName = name!;
        vm.HeadlineLocation = location!;

        await vm.HeadlineChangeCommand.ExecuteAsync(null);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Headline needs a name and a location", vm.LastActionStatus);
    }

    [Fact]
    public void HeadlineFields_SeedFromSnapshot_WhenBothEmpty()
    {
        var vm = NewVm();
        Assert.Equal("", vm.HeadlineName);
        Assert.Equal("", vm.HeadlineLocation);

        vm.OnSnapshot(Snapshot(GfxSnapshot(headlineName: "Ann", headlineLocation: "Turku")));

        Assert.Equal("Ann", vm.HeadlineName);
        Assert.Equal("Turku", vm.HeadlineLocation);
    }

    [Fact]
    public void HeadlineFields_DoNotClobberAnOperatorsInProgressEdit()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(revision: "10", headlineName: "Ann", headlineLocation: "Turku")));
        Assert.Equal("Ann", vm.HeadlineName);
        Assert.Equal("Turku", vm.HeadlineLocation);

        // Operator starts typing a new name — a later snapshot must not stomp it.
        vm.HeadlineName = "Someone Else";

        vm.OnSnapshot(Snapshot(GfxSnapshot(revision: "11", headlineName: "Cid", headlineLocation: "Vaasa")));

        Assert.Equal("Someone Else", vm.HeadlineName);
    }

    // ── question / mukana ───────────────────────────────────────────────────────────────

    [Fact]
    public async Task QuestionIn_SendsQuestionIn()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.QuestionInCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gfx.question.in", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task QuestionOut_SendsQuestionOut()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.QuestionOutCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.gfx.question.out", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public async Task MukanaSync_SendsMukanaSync()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);

        await vm.MukanaSyncCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.mukana.sync", actionId);
        Assert.Empty(args);
    }

    [Fact]
    public void QuestionTextAndAsker_ComeFromOverlays()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot()));

        Assert.Equal("What is your name?", vm.QuestionText);
        Assert.Equal("Bob", vm.QuestionAsker);
    }

    [Fact]
    public void HeadlineVisible_ComesFromOverlays()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(headlineVisible: true)));
        Assert.True(vm.HeadlineVisible);

        vm.OnSnapshot(Snapshot(GfxSnapshot(revision: "11", headlineVisible: false)));
        Assert.False(vm.HeadlineVisible);
    }

    // ── override editor ─────────────────────────────────────────────────────────────────

    [Fact]
    public async Task OverrideSet_SendsMukanaOverrideSet()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OverridePin = "1001";
        vm.OverrideName = "Ann";
        vm.OverrideLocation = "Helsinki";
        vm.OverrideRole = "host";

        await vm.OverrideSetCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.mukana.override.set", actionId);
        Assert.Equal(new object?[] { "1001", "Ann", "Helsinki", "host" }, args);
    }

    [Theory]
    [InlineData("", "Ann", "Helsinki", "host")]
    [InlineData("1001", "", "Helsinki", "host")]
    [InlineData("1001", "Ann", "", "host")]
    [InlineData("1001", "Ann", "Helsinki", "")]
    public async Task OverrideSet_MissingField_RefusesLocally_NeverInvokes(
        string pin, string name, string location, string role)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OverridePin = pin;
        vm.OverrideName = name;
        vm.OverrideLocation = location;
        vm.OverrideRole = role;

        await vm.OverrideSetCommand.ExecuteAsync(null);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Override needs PIN, name, location, and role", vm.LastActionStatus);
    }

    [Fact]
    public void OverrideRole_DefaultsToPanelist()
    {
        var vm = NewVm();
        Assert.Equal("panelist", vm.OverrideRole);
    }

    [Fact]
    public async Task OverrideDelete_SendsMukanaOverrideDelete()
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OverridePin = "1001";

        await vm.OverrideDeleteCommand.ExecuteAsync(null);

        var (actionId, args) = Assert.Single(invoker.Invocations);
        Assert.Equal("ohg.mukana.override.delete", actionId);
        Assert.Equal(new object?[] { "1001" }, args);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    public async Task OverrideDelete_NoPin_RefusesLocally_NeverInvokes(string? pin)
    {
        var invoker = new FakeOhgActionInvoker();
        var vm = NewVm(invoker);
        vm.OverridePin = pin!;

        await vm.OverrideDeleteCommand.ExecuteAsync(null);

        Assert.Empty(invoker.Invocations);
        Assert.Equal("Override delete needs a PIN", vm.LastActionStatus);
    }

    // ── lamps + health label ─────────────────────────────────────────────────────────────

    [Theory]
    [InlineData("available")]
    [InlineData("unavailable")]
    [InlineData("disabled")]
    public void RegistryLamp_ReflectsRegistryCapabilityState(string state)
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(registryState: state, registryDetail: "some detail")));

        Assert.Equal(state, vm.RegistryLamp);
        Assert.Equal("some detail", vm.RegistryLampDetail);
    }

    [Theory]
    [InlineData("available")]
    [InlineData("unavailable")]
    [InlineData("disabled")]
    public void HandsLamp_ReflectsHandsQueueCapabilityState(string state)
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(handsState: state)));

        Assert.Equal(state, vm.HandsLamp);
    }

    [Theory]
    [InlineData("available")]
    [InlineData("unavailable")]
    [InlineData("disabled")]
    public void QuestionLamp_ReflectsQuestionFeedCapabilityState(string state)
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(questionState: state)));

        Assert.Equal(state, vm.QuestionLamp);
    }

    [Fact]
    public void MukanaHealthLabel_IsHealthWorst()
    {
        var vm = NewVm();
        vm.OnSnapshot(Snapshot(GfxSnapshot(worst: "dormant")));

        Assert.Equal("dormant", vm.MukanaHealthLabel);
    }
}
