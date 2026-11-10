using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Converters;
using CoreVideoPro.WinUI.ViewModels.Comms;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The Comms page view model (docs/reference/comms-talkback.md): the JSON each action sends,
/// the screen states, and the structure gate that keeps a held key alive across updates.
/// Mirrors CoreVideo Edge's CommsViewModelTests so the two apps keep one behaviour.
/// </summary>
public sealed class CommsViewModelTests
{
    private sealed class FakeTransport : IZoomCommsTransport
    {
        public event Action<ZoomCommsUpdate>? ZoomCommsStateReceived;
        public List<(string Verb, string Json)> Sent { get; } = new();
        public ZoomCommsCommandResult Reply { get; set; } = ZoomCommsCommandResult.Success;

        public Task<ZoomCommsCommandResult> SendCommsAsync(string verb, IReadOnlyDictionary<string, object?> payload,
            CancellationToken cancellationToken = default)
        {
            Sent.Add((verb, JsonSerializer.Serialize(payload.OrderBy(p => p.Key).ToDictionary(p => p.Key, p => p.Value))));
            return Task.FromResult(Reply);
        }

        public void Raise(ZoomCommsUpdate update) => ZoomCommsStateReceived?.Invoke(update);
    }

    private static ZoomCommsState Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return ZoomCommsProtocol.ParseState(doc.RootElement);
    }

    private static string Channel(int slot, string? label = null, int listeners = 0, bool ready = true, bool keyed = false,
        bool latched = false, string? room = null, int reachOk = -1, int reachDark = 0) =>
        $$"""{"slot":{{slot}},"label":{{(label is null ? "null" : $"\"{label}\"")}},"listeners":{{listeners}},"ready":{{(ready ? "true" : "false")}},"keyed":{{(keyed ? "true" : "false")}},"latched":{{(latched ? "true" : "false")}},"room":{{(room is null ? "null" : $"\"{room}\"")}},"reachOk":{{(reachOk < 0 ? listeners : reachOk)}},"reachDark":{{reachDark}}}""";

    private static string Desk(string channels, string roster = "[]", string extra = "") =>
        $$"""{"available":true,"reason":null,"lamps":{"link":true,"meetingMic":true,"channel":true,"tx":false},"room":null,"channels":[{{channels}}],"roster":{{roster}},"mic":{"device":"Mic A","devices":["Mic A","Mic B"],"peakDbfs":-24,"gainDb":0,"sidetone":true,"sidetoneDevice":"Phones","sidetoneDevices":["Phones","Speakers"],"tone":false},"stats":{"framesSent":10,"channelSends":4,"underruns":0,"sendErrors":0},"log":["a","b"]{{extra}}}""";

    private static (CommsViewModel Vm, FakeTransport Transport) Build(string? comms = null, bool inMeeting = true)
    {
        var transport = new FakeTransport();
        var vm = new CommsViewModel(transport);
        vm.ApplyMeeting(inMeeting, "Meeting 123 456 7890");
        if (comms is not null) vm.ApplyComms(new ZoomCommsUpdate(Parse(comms)));
        return (vm, transport);
    }

    private static readonly string TwoPeople = Desk(Channel(0, "Ana", 1) + "," + Channel(1, "Ben", 1) + "," + Channel(2));

    [Fact]
    public void ParsesTheCoreCommsFact()
    {
        Assert.True(ZoomCommsProtocol.TryParseStateLine(
            """{"type":"zoom-comms-state","comms":""" + TwoPeople + "}", out var update));
        Assert.NotNull(update.State);
        Assert.Equal(3, update.State!.Channels.Count);
        Assert.Equal("Ana", update.State.Channels[0].Label);
        Assert.True(update.State.LampMeetingMic);
        Assert.Equal(-24, update.State.Mic.PeakDbfs);
        Assert.Equal(["Phones", "Speakers"], update.State.Mic.SidetoneDevices);

        Assert.True(ZoomCommsProtocol.TryParseStateLine("""{"type":"zoom-comms-state","comms":null}""", out var cleared));
        Assert.Null(cleared.State);
        Assert.False(ZoomCommsProtocol.TryParseStateLine("""{"id":"7","type":"zoom-comms-state","comms":null}""", out _));
        Assert.False(ZoomCommsProtocol.TryParseStateLine("""{"type":"zoom-source-format"}""", out _));
    }

    [Fact]
    public void ScreenStatesAreExplicit()
    {
        var (vm, _) = Build(inMeeting: false);
        Assert.True(vm.NotInMeeting);
        Assert.False(vm.DeskVisible);
        Assert.Equal("Not in a meeting", vm.RailText);

        vm.ApplyMeeting(true, "Meeting 123 456 7890");
        Assert.True(vm.Waiting);  // in the meeting, no report yet: waiting, not unavailable
        Assert.False(vm.Unavailable);

        vm.ApplyComms(new ZoomCommsUpdate(Parse(
            """{"available":false,"reason":"Make this account host or co-host to bring talkback up.","lamps":{"link":true}}""")));
        Assert.True(vm.Unavailable);
        Assert.True(vm.HostHintVisible);
        Assert.True(vm.LampLink);  // lamps are machine truth even when refused
        Assert.False(vm.LampChannel);
        Assert.Equal("Make this account host or co-host to bring talkback up.", vm.UnavailableText);

        vm.ApplyComms(new ZoomCommsUpdate(Parse(TwoPeople)));
        Assert.True(vm.DeskVisible);
        Assert.True(vm.GridVisible);
        Assert.Equal("Meeting 123 456 7890", vm.RailText);

        vm.ApplyComms(new ZoomCommsUpdate(null));  // the core dropped the report
        Assert.True(vm.Waiting);
        Assert.Empty(vm.Cells);
    }

    [Fact]
    public void OnlyOccupiedNamedOrKeyedChannelsAreCells()
    {
        var (vm, _) = Build(Desk(Channel(0, "Ana", 1) + "," + Channel(1) + "," + Channel(2, null, 0, keyed: true) + "," + Channel(3)));
        Assert.Equal([0, 2], vm.Cells.Select(c => c.Slot));
        Assert.Equal("CH 3", vm.Cells[1].Name);
    }

    [Fact]
    public async Task HoldingACellKeysItWithALeaseAndReleasingUnkeys()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.PressAsync(vm.Cells[0]);
        await vm.PressAsync(vm.Cells[0]);  // a second press while held sends nothing
        await vm.ReleaseAsync(vm.Cells[0]);
        await vm.ReleaseAsync(vm.Cells[0]);  // a second release sends nothing
        Assert.Equal(
            [("comms-key", """{"leaseMs":1500,"on":true,"slot":0}"""), ("comms-key", """{"on":false,"slot":0}""")],
            t.Sent);
    }

    [Fact]
    public void OnAirComesFromTheEngineOnly()
    {
        var (vm, _) = Build(TwoPeople);
        _ = vm.PressAsync(vm.Cells[0]);
        Assert.Equal(CommsTone.Ready, vm.Cells[0].Tone);  // pressed, but the engine has not said keyed
        Assert.Equal("ready", vm.Cells[0].StateText);
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 1, keyed: true) + "," + Channel(1, "Ben", 1)))));
        Assert.Equal(CommsTone.Hot, vm.Cells[0].Tone);
        Assert.Equal("on air", vm.Cells[0].StateText);
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 0, keyed: true) + "," + Channel(1, "Ben", 1)))));
        Assert.Equal(CommsTone.Armed, vm.Cells[0].Tone);  // keyed, nobody hearing: never hot
        Assert.Equal("nobody in channel", vm.Cells[0].StateText);
    }

    [Fact]
    public async Task LatchModeTogglesInsteadOfHolding()
    {
        var (vm, t) = Build(TwoPeople);
        vm.ToggleLatchModeCommand.Execute(null);
        await vm.PressAsync(vm.Cells[1]);
        await vm.ReleaseAsync(vm.Cells[1]);  // release does nothing in latch mode
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 1) + "," + Channel(1, "Ben", 1, keyed: true, latched: true)))));
        Assert.Equal("on air · latched", vm.Cells[1].StateText);
        await vm.PressAsync(vm.Cells[1]);
        Assert.Equal(
            [("comms-latch", """{"on":true,"slot":1}"""), ("comms-latch", """{"on":false,"slot":1}""")],
            t.Sent);
    }

    [Fact]
    public async Task AllCallKeysEveryChannelAndLatchesAll()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.AllPressAsync();
        await vm.AllPressAsync();
        await vm.AllReleaseAsync();
        vm.LatchMode = true;
        await vm.AllPressAsync();
        Assert.Equal(
            [
                ("comms-key", """{"all":true,"leaseMs":1500,"on":true}"""),
                ("comms-key", """{"all":true,"on":false}"""),
                ("comms-latch", """{"all":true,"on":true}"""),
            ],
            t.Sent);
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 1, keyed: true) + "," + Channel(1, "Ben", 0, keyed: true)))));
        Assert.True(vm.AllHot);
        Assert.Equal("ALL CALL · ON AIR", vm.AllText);
    }

    [Fact]
    public void AnUpdateThatKeepsTheSameCellsNeverRebuildsThem()
    {
        var (vm, _) = Build(TwoPeople);
        var first = vm.Cells[0];
        var version = vm.CellStructureVersion;
        var changes = 0;
        vm.Cells.CollectionChanged += (_, _) => changes++;
        _ = vm.PressAsync(first);
        // Level, keyed and listener changes are values, not structure.
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 1, keyed: true) + "," + Channel(1, "Ben", 1) + "," + Channel(2)))));
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 0, keyed: true) + "," + Channel(1, "Ben", 1) + "," + Channel(2)))));
        Assert.Same(first, vm.Cells[0]);
        Assert.Equal(version, vm.CellStructureVersion);
        Assert.Equal(0, changes);
        Assert.True(first.Held);
    }

    [Fact]
    public void AStructureChangeKeepsSurvivingCellsAndTheirHold()
    {
        var (vm, _) = Build(TwoPeople);
        var ben = vm.Cells[1];
        _ = vm.PressAsync(ben);
        var resets = 0;
        vm.Cells.CollectionChanged += (_, e) => { if (e.Action == System.Collections.Specialized.NotifyCollectionChangedAction.Reset) resets++; };
        // Ana leaves channel 0: the set changes, Ben's key must survive with its hold.
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0) + "," + Channel(1, "Ben", 1) + "," + Channel(2)))));
        Assert.Single(vm.Cells);
        Assert.Same(ben, vm.Cells[0]);
        Assert.True(ben.Held);
        Assert.Equal(0, resets);
    }

    [Fact]
    public async Task ReleaseAllReleasesEveryHeldKey()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.PressAsync(vm.Cells[0]);
        await vm.AllPressAsync();
        t.Sent.Clear();
        await vm.ReleaseAllAsync();
        Assert.Equal(
            [("comms-key", """{"on":false,"slot":0}"""), ("comms-key", """{"all":true,"on":false}""")],
            t.Sent);
        Assert.False(vm.AnyHeld);
    }

    [Fact]
    public async Task AHeldKeyRenewsItsLeaseAndAReleasedOneDoesNot()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.PressAsync(vm.Cells[1]);
        t.Sent.Clear();
        await vm.RenewLeasesAsync();
        Assert.Equal([("comms-key", """{"leaseMs":1500,"on":true,"slot":1}""")], t.Sent);
        await vm.ReleaseAsync(vm.Cells[1]);
        t.Sent.Clear();
        await vm.RenewLeasesAsync();
        Assert.Empty(t.Sent);
    }

    [Fact]
    public async Task LatchedChannelsAreNotLeased()
    {
        var (vm, t) = Build(TwoPeople);
        vm.LatchMode = true;
        await vm.PressAsync(vm.Cells[0]);
        t.Sent.Clear();
        await vm.RenewLeasesAsync();
        Assert.Empty(t.Sent);
        Assert.False(vm.AnyHeld);
    }

    [Fact]
    public async Task ShutdownSendsReleaseAll()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.PressAsync(vm.Cells[0]);
        t.Sent.Clear();
        await vm.SendReleaseAllAsync();
        Assert.Equal([("comms-release-all", "{}")], t.Sent);
        Assert.False(vm.AnyHeld);
    }

    [Fact]
    public async Task LeavingTheMeetingForgetsHeldKeysLocally()
    {
        var (vm, t) = Build(TwoPeople);
        await vm.PressAsync(vm.Cells[0]);
        vm.ApplyMeeting(false, null);
        Assert.False(vm.AnyHeld);
        Assert.True(vm.NotInMeeting);
        t.Sent.Clear();
        await vm.RenewLeasesAsync();
        Assert.Empty(t.Sent);
    }

    [Fact]
    public void SomeoneInAnotherRoomCannotBeKeyed()
    {
        var (vm, t) = Build(Desk(Channel(0, "Ana", 1, room: "Green room") + "," + Channel(1, "Ben", 1)));
        var ana = vm.Cells[0];
        Assert.True(ana.Off);
        Assert.Equal(CommsTone.Off, ana.Tone);
        Assert.Equal("in Green room", ana.StateText);
        _ = vm.PressAsync(ana);
        Assert.Empty(t.Sent);
        Assert.Same(vm.Cells[1], vm.CellForDigit(1));  // digits skip unreachable cells
        Assert.Equal("1", vm.Cells[1].KeyHint);
    }

    [Fact]
    public void AGroupSaysHowManyListenAndWhoIsElsewhere()
    {
        var (vm, _) = Build(Desk(Channel(4, null, 3, reachOk: 3, reachDark: 2)));
        Assert.Equal("3 listening · 2 elsewhere", vm.Cells[0].StateText);
        Assert.Equal("CH 5", vm.Cells[0].Name);
    }

    [Fact]
    public async Task EditTalentChipsAssignAndExplainWebClients()
    {
        var roster = """[{"participantId":201,"name":"Ana","talkback":true,"room":null,"channels":[0]},{"participantId":203,"name":"Wes","talkback":false,"room":null,"channels":[]}]""";
        var (vm, t) = Build(Desk(Channel(0, "Ana", 1) + "," + Channel(1), roster));
        vm.ToggleEditCommand.Execute(null);
        Assert.True(vm.EditVisible);
        Assert.False(vm.GridVisible);
        Assert.Equal(2, vm.People.Count);
        var ana = vm.People[0];
        Assert.Equal(2, ana.Chips.Count);  // one chip per channel
        Assert.True(ana.Chips[0].On);
        Assert.Equal("Take off Ana", ana.Chips[0].Tip);
        Assert.False(ana.Chips[1].On);
        Assert.Equal("Put on CH 2 (empty)", ana.Chips[1].Tip);
        var wes = vm.People[1];
        Assert.True(wes.NoTalkback);
        Assert.Empty(wes.Chips);
        Assert.Contains("web client", CommsViewModel.WebClientWhy);

        await ((CommunityToolkit.Mvvm.Input.IAsyncRelayCommand)ana.Chips[1].Command).ExecuteAsync(null);
        await vm.LastSend;
        Assert.Equal([("comms-assign", """{"on":true,"participantId":201,"slot":1}""")], t.Sent);
    }

    [Fact]
    public void PeopleRowsAreRebuiltOnlyWhenTheRosterChanges()
    {
        var roster = """[{"participantId":201,"name":"Ana","talkback":true,"room":null,"channels":[0]}]""";
        var (vm, _) = Build(Desk(Channel(0, "Ana", 1), roster));
        var row = vm.People[0];
        var version = vm.PeopleStructureVersion;
        vm.ApplyComms(new ZoomCommsUpdate(Parse(Desk(Channel(0, "Ana", 1, keyed: true), roster))));
        Assert.Equal(version, vm.PeopleStructureVersion);
        Assert.Same(row, vm.People[0]);
    }

    [Fact]
    public async Task SettingsSendTheirVerbs()
    {
        var (vm, t) = Build(TwoPeople);
        vm.SelectedMic = "Mic B";
        await vm.LastSend;
        vm.SelectedOutput = "Speakers";
        await vm.LastSend;
        await vm.SidetoneCommand.ExecuteAsync(null);
        await vm.ToneCommand.ExecuteAsync(null);
        await vm.GainUpCommand.ExecuteAsync(null);
        await vm.GainDownCommand.ExecuteAsync(null);
        Assert.Equal(
            [
                ("comms-mic", """{"device":"Mic B"}"""),
                ("comms-sidetone-out", """{"device":"Speakers"}"""),
                ("comms-sidetone", """{"on":false}"""),
                ("comms-tone", """{"on":true}"""),
                ("comms-gain", """{"db":1}"""),
                ("comms-gain", """{"db":-1}"""),
            ],
            t.Sent);
        Assert.Equal("Mic A", vm.SelectedMic);  // still the engine's until it reports the switch
    }

    [Fact]
    public void ApplyingStateNeverSendsADeviceChangeOrRebuildsThePickers()
    {
        var (vm, t) = Build(TwoPeople);
        var micChanges = 0;
        vm.Mics.CollectionChanged += (_, _) => micChanges++;
        vm.ApplyComms(new ZoomCommsUpdate(Parse(TwoPeople)));
        vm.ApplyComms(new ZoomCommsUpdate(Parse(TwoPeople.Replace("\"peakDbfs\":-24", "\"peakDbfs\":-12"))));
        Assert.Empty(t.Sent);
        Assert.Equal(0, micChanges);
        Assert.Equal(["Mic A", "Mic B"], vm.Mics);
    }

    [Fact]
    public async Task ARefusalIsShownVerbatim()
    {
        var (vm, t) = Build(TwoPeople);
        t.Reply = new ZoomCommsCommandResult(false, "Comms is not available: Not in a meeting.");
        await vm.PressAsync(vm.Cells[0]);
        Assert.Equal("Comms is not available: Not in a meeting.", vm.Error);
        Assert.True(vm.ErrorVisible);
        t.Reply = ZoomCommsCommandResult.Success;
        await vm.ReleaseAsync(vm.Cells[0]);
        Assert.Null(vm.Error);
    }

    [Fact]
    public void MeterLightsOnlyFromAMeasuredLevel()
    {
        var (vm, _) = Build(TwoPeople);  // -24 dBFS: half of 12 segments
        Assert.Equal(6, vm.Meter.Count(s => s.Lit));
        vm.ApplyComms(new ZoomCommsUpdate(Parse(TwoPeople.Replace("\"peakDbfs\":-24", "\"peakDbfs\":null"))));
        Assert.Equal(0, vm.Meter.Count(s => s.Lit));
        Assert.Equal("Microphone level not measured", vm.MicLevelName);
    }

    [Fact]
    public void OpsHistoryIsNewestFirstAndSyncedInPlace()
    {
        var (vm, _) = Build(TwoPeople);
        Assert.Equal("b", vm.OpsLine);
        Assert.Equal(["b", "a"], vm.OpsLines.Select(e => e.Text));
        var changes = 0;
        vm.OpsLines.CollectionChanged += (_, _) => changes++;
        vm.ApplyComms(new ZoomCommsUpdate(Parse(TwoPeople)));  // same log: nothing moves
        Assert.Equal(0, changes);
    }

    [Theory]
    [InlineData("https://zoom.us/j/1234567890?pwd=abc", "Meeting 123 456 7890")]
    [InlineData("https://us02web.zoom.us/j/12345678901", "Meeting 123 4567 8901")]
    [InlineData("123 456 7890", "Meeting 123 456 7890")]
    [InlineData("", null)]
    [InlineData("https://zoom.us/my/someone", null)]
    public void MeetingLabel(string url, string? expected) => Assert.Equal(expected, CommsViewModel.MeetingLabelFor(url));

    [Fact]
    public void OnAirUsesProsAirRedAndNoRawColours()
    {
        Assert.Equal("StudioAirBrush", CommsBrushConverter.ForTone(CommsTone.Hot, "cell-bg"));
        Assert.Equal("StudioBackgroundBrush", CommsBrushConverter.ForTone(CommsTone.Hot, "cell-fg"));
        Assert.Equal("StudioWarningBrush", CommsBrushConverter.ForTone(CommsTone.Armed, "cell-border"));
        Assert.NotEqual("StudioAirBrush", CommsBrushConverter.ForTone(CommsTone.Armed, "cell-bg"));
        Assert.Equal("StudioLiveBrush", CommsBrushConverter.ForBool(true, "lamp"));
        foreach (var tone in Enum.GetValues<CommsTone>())
            foreach (var part in new[] { "cell-bg", "cell-border", "cell-fg", "meter" })
                Assert.StartsWith("Studio", CommsBrushConverter.ForTone(tone, part));
    }
}
