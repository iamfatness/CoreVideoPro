using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels.Comms;

/// <summary>How a talk key reads. Mapped to Pro's status tokens by the page
/// (<c>CommsToneBrushConverter</c>): Hot = StudioAirBrush (Pro's on-air red),
/// Armed = StudioWarningBrush, Ready = StudioTextBrush, Idle = StudioTextSecondaryBrush,
/// Off = StudioDimBrush. Never Hot unless the engine reports the channel keyed AND
/// someone confirmed in it.</summary>
public enum CommsTone
{
    Idle,
    Ready,
    Armed,
    Hot,
    Off,
}

/// <summary>One talk key: a person (or a group channel). The cell IS the key (ZComms
/// model). Its instance survives every state update (and structure changes that keep its
/// slot), so a held press never loses its release.</summary>
public sealed partial class CommsCellViewModel : ObservableObject
{
    public CommsCellViewModel(int slot) { Slot = slot; }

    public int Slot { get; }

    /// <summary>Pressed by this operator and not yet released (pointer or digit key).</summary>
    public bool Held { get; internal set; }

    [ObservableProperty, NotifyPropertyChangedFor(nameof(AutomationName))] private string name = "";
    [ObservableProperty, NotifyPropertyChangedFor(nameof(AutomationName))] private string stateText = "";
    [ObservableProperty] private CommsTone tone = CommsTone.Idle;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(Enabled))] private bool off;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(KeyHintVisible))] private string keyHint = "";

    public bool Enabled => !Off;
    public bool KeyHintVisible => KeyHint.Length > 0;
    public string AutomationName => $"Talk to {Name}: {StateText}";
}

public sealed class CommsChipViewModel
{
    public required int Slot { get; init; }
    public required string Text { get; init; }
    public required bool On { get; init; }
    public required bool Busy { get; init; }
    public required string Tip { get; init; }
    public required IRelayCommand Command { get; init; }
}

public sealed partial class CommsPersonViewModel : ObservableObject
{
    public CommsPersonViewModel(uint participantId) { ParticipantId = participantId; }

    public uint ParticipantId { get; }
    [ObservableProperty] private string name = "";
    [ObservableProperty, NotifyPropertyChangedFor(nameof(NoTalkback))] private bool talkback;
    [ObservableProperty] private IReadOnlyList<CommsChipViewModel> chips = [];
    public bool NoTalkback => !Talkback;
}

/// <summary>One ops-log line in the history (Key keeps repeated lines distinct).</summary>
public sealed record CommsOpsEntry(string Key, string Text);

public sealed partial class CommsMeterSegment : ObservableObject
{
    public required CommsTone Tone { get; init; }
    [ObservableProperty] private bool lit;
}

/// <summary>
/// The Comms page (Zoom talkback, docs/reference/comms-talkback.md): the ZComms talkback
/// station driven by the core's comms report. Behaviour is CoreVideo Edge's
/// <c>CommsViewModel</c> (same verbs, same JSON, same screen states); only the look is Pro's.
/// Constructible without XAML so the JSON it sends and its states are unit-tested.
/// </summary>
public sealed partial class CommsViewModel : ObservableObject
{
    public const int MeterSegments = 12;
    /// <summary>A held key is leased: the engine drops it unless re-sent within LeaseMs,
    /// so a hung shell can never leave a live mic on talent (Edge contract, comms-key
    /// leaseMs). The shell calls <see cref="RenewLeasesAsync"/> every RenewEvery.</summary>
    public const int LeaseMs = 1500;
    public static readonly TimeSpan RenewEvery = TimeSpan.FromMilliseconds(500);
    public const string WebClientWhy = "On the Zoom web client. Ask them to rejoin in the Zoom desktop app.";

    private readonly IZoomCommsTransport _transport;
    private readonly Func<long> _clockMs;
    private bool _privatePreference;
    private long _privateSentAtMs = long.MinValue / 2;
    /// <summary>A private re-send waits this long for the engine to report it.</summary>
    public const long PrivateResendMs = 2000;
    private ZoomCommsState? _comms;
    private bool _inMeeting;
    private string? _meetingLabel;
    private string _cellSig = "";
    private string _peopleSig = "";
    private string _micsSig = "";
    private string _outsSig = "";
    private bool _allHeld;
    private bool _applying;

    public CommsViewModel(IZoomCommsTransport transport, Func<long>? clockMs = null)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
        _clockMs = clockMs ?? (() => Environment.TickCount64);
        for (var i = 0; i < MeterSegments; i++)
            Meter.Add(new CommsMeterSegment { Tone = i == MeterSegments - 1 ? CommsTone.Hot : i >= 8 ? CommsTone.Armed : CommsTone.Ready });
    }

    public ObservableCollection<CommsCellViewModel> Cells { get; } = new();
    public ObservableCollection<CommsPersonViewModel> People { get; } = new();
    public ObservableCollection<CommsMeterSegment> Meter { get; } = new();
    public ObservableCollection<string> Mics { get; } = new();
    public ObservableCollection<string> Outputs { get; } = new();

    /// <summary>Tests await the last send.</summary>
    public Task LastSend { get; private set; } = Task.CompletedTask;

    // ------------------------------------------------------------ screen states

    public bool InMeeting => _inMeeting;
    public bool NotInMeeting => !_inMeeting;
    public bool Waiting => _inMeeting && _comms is null;
    public bool Unavailable => _inMeeting && _comms is { Available: false };
    public bool DeskVisible => _inMeeting && _comms is { Available: true };
    public string UnavailableText => _comms?.Reason ?? "Talkback is not available.";
    public bool HostHintVisible => Unavailable && (_comms?.Reason ?? "").Contains("host", StringComparison.OrdinalIgnoreCase);

    public bool GridVisible => DeskVisible && !EditMode && Cells.Count > 0;
    public bool EditVisible => DeskVisible && EditMode;
    public bool EmptyDeskVisible => DeskVisible && !EditMode && Cells.Count == 0;
    public string EmptyDeskText => _comms?.Roster.Count > 0 ? "In the meeting. Channels forming…" : "In the meeting. Waiting for participants…";

    // ------------------------------------------------------------ rail

    public string RailText
    {
        get
        {
            if (!_inMeeting) return "Not in a meeting";
            var room = (_comms?.Room is string r ? $" · Room {r}" : "") + (PrivateReported ? " · private comms" : "");
            return (_meetingLabel is { Length: > 0 } label ? label : "In meeting") + room;
        }
    }

    // Lamps are machine truth even when talkback is refused (LINK can be up while
    // CHANNEL never comes up), so they follow the report, not the desk.
    private bool Reported => _inMeeting && _comms is not null;
    public bool LampLink => Reported && _comms!.LampLink;
    public bool LampMic => Reported && _comms!.LampMeetingMic;
    public bool LampChannel => Reported && _comms!.LampChannel;
    public bool LampTx => Reported && _comms!.LampTx;

    // ------------------------------------------------------------ all call

    private bool AllKeyed => DeskVisible && _comms!.Channels.Count > 0 && _comms.Channels.All(c => c.Keyed);
    public bool AllHot => AllKeyed && _comms!.Channels.Any(c => c.Listeners > 0);
    public bool AllArmed => AllKeyed && !_comms!.Channels.Any(c => c.Listeners > 0);
    public CommsTone AllTone => AllHot ? CommsTone.Hot : AllArmed ? CommsTone.Armed : CommsTone.Ready;
    public string AllText => AllHot ? (_comms!.Channels.All(c => c.Latched) ? "ALL CALL · ON AIR · LATCHED" : "ALL CALL · ON AIR") : "ALL CALL";

    // ------------------------------------------------------------ strip

    [ObservableProperty] private bool latchMode;
    [ObservableProperty] private bool editMode;
    [ObservableProperty] private bool settingsOpen;
    [ObservableProperty] private bool opsOpen;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(ErrorVisible))] private string? error;
    public bool ErrorVisible => Error is not null;

    partial void OnEditModeChanged(bool value) => RaiseDesk();
    partial void OnOpsOpenChanged(bool value)
    {
        OnPropertyChanged(nameof(OpsLine));
        OnPropertyChanged(nameof(OpsLineVisible));
    }

    [RelayCommand] private void ToggleLatchMode() => LatchMode = !LatchMode;
    [RelayCommand] private void ToggleEdit() => EditMode = !EditMode;
    [RelayCommand] private void ToggleSettings() => SettingsOpen = !SettingsOpen;
    [RelayCommand] private void ToggleOps() => OpsOpen = !OpsOpen;

    // ------------------------------------------------------------ settings

    /// <summary>The device in use as the engine reports it. Setting it (from the picker)
    /// sends comms-mic; the value shown stays the engine's until it reports the switch.</summary>
    public string? SelectedMic
    {
        get => _comms?.Mic.Device;
        set { if (!_applying && value is not null && value != _comms?.Mic.Device) _ = SendAsync("comms-mic", new() { ["device"] = value }); }
    }

    public string? SelectedOutput
    {
        get => _comms?.Mic.SidetoneDevice;
        set { if (!_applying && value is not null && value != _comms?.Mic.SidetoneDevice) _ = SendAsync("comms-sidetone-out", new() { ["device"] = value }); }
    }

    public bool SidetoneOn => _comms?.Mic.Sidetone == true;
    public bool ToneOn => _comms?.Mic.Tone == true;
    public string GainText => _comms is { } c ? $"{(c.Mic.GainDb > 0 ? "+" : "")}{c.Mic.GainDb} dB" : "";
    public string FramesSent => _comms?.FramesSent.ToString("N0") ?? "";
    public string ChannelSends => _comms?.ChannelSends.ToString("N0") ?? "";
    public string Underruns => _comms?.Underruns.ToString("N0") ?? "";
    public string SendErrors => _comms?.SendErrors.ToString("N0") ?? "";
    public bool SendErrorsWarn => _comms?.SendErrors > 0;
    public double? MicDb => DeskVisible ? _comms!.Mic.PeakDbfs : null;
    public string MicLevelName => MicDb is double db ? $"Microphone level {db:0} dBFS" : "Microphone level not measured";

    [RelayCommand] private Task Sidetone() => SendAsync("comms-sidetone", new() { ["on"] = !SidetoneOn });
    [RelayCommand] private Task Tone() => SendAsync("comms-tone", new() { ["on"] = !ToneOn });
    [RelayCommand] private Task GainDown() => SendGainAsync(-1);
    [RelayCommand] private Task GainUp() => SendGainAsync(+1);

    // ------------------------------------------------------------ private comms

    /// <summary>The operator's "Private comms" choice (persisted by the shell; OFF by
    /// default so existing shows are unchanged). ON: the meeting mic becomes the silent
    /// virtual mic and the operator speaks only through talkback. OFF: their own mic,
    /// which talkback needs open; turning it off leaves them MUTED.</summary>
    public bool PrivatePreference => _privatePreference;

    /// <summary>What the engine applies (comms.private).</summary>
    public bool PrivateReported => _comms?.Private == true;

    public string PrivateHint => _privatePreference
        ? "Private comms ON: the room never hears you; you speak only through talkback. Turn off to use your own meeting mic (you stay muted until you unmute)."
        : "Private comms OFF: talkback uses your own meeting mic, which must be open (the room hears it too). Turn on so only talent hears you.";

    /// <summary>Raised when the operator changes the preference (the shell persists it).</summary>
    public event Action<bool>? PrivatePreferenceChanged;

    /// <summary>Startup: the saved preference, without sending anything yet.</summary>
    public void RestorePrivatePreference(bool on)
    {
        if (_privatePreference == on) return;
        _privatePreference = on;
        RaisePrivate();
    }

    [RelayCommand]
    private Task TogglePrivate()
    {
        _privatePreference = !_privatePreference;
        RaisePrivate();
        PrivatePreferenceChanged?.Invoke(_privatePreference);
        return _inMeeting ? SendPrivateAsync() : Task.CompletedTask;
    }

    private Task SendPrivateAsync()
    {
        _privateSentAtMs = _clockMs();
        return SendAsync("comms-private", new() { ["on"] = _privatePreference });
    }

    // The engine forgets nothing within a process, but a new engine (rejoin, core
    // restart) starts OFF: re-assert the preference whenever a report disagrees,
    // at most once per PrivateResendMs (never a storm at the report rate).
    private void ReconcilePrivate()
    {
        if (_comms is null || !_inMeeting || _comms.Private == _privatePreference) return;
        if (_clockMs() - _privateSentAtMs < PrivateResendMs) return;
        _ = SendPrivateAsync();
    }

    private void RaisePrivate()
    {
        OnPropertyChanged(nameof(PrivatePreference));
        OnPropertyChanged(nameof(PrivateHint));
    }

    // ------------------------------------------------------------ ops strip

    public string OpsLine => _comms?.Log.Count > 0 ? _comms.Log[^1] : "No comms events yet.";
    /// <summary>Newest first. Synced in place only when the log changes, so an open
    /// history is never rebuilt at the 10 Hz report rate.</summary>
    public ObservableCollection<CommsOpsEntry> OpsLines { get; } = new();
    public bool OpsLineVisible => !OpsOpen;

    /// <summary>"Meeting 123 4567 8901" from a join URL or bare id; null when there is no id.</summary>
    public static string? MeetingLabelFor(string? joinUrlOrId)
    {
        if (string.IsNullOrWhiteSpace(joinUrlOrId)) return null;
        var match = System.Text.RegularExpressions.Regex.Match(joinUrlOrId, @"(?:/j/|/w/|^)\s*(\d[\d\s-]{7,})");
        if (!match.Success) return null;
        var digits = new string(match.Groups[1].Value.Where(char.IsDigit).ToArray());
        if (digits.Length < 9) return null;
        // Zoom's own grouping: 3-3-4 for 10 digits, 3-4-4 for 11.
        var grouped = digits.Length switch
        {
            10 => $"{digits[..3]} {digits[3..6]} {digits[6..]}",
            11 => $"{digits[..3]} {digits[3..7]} {digits[7..]}",
            _ => digits,
        };
        return $"Meeting {grouped}";
    }

    // ------------------------------------------------------------ apply

    /// <summary>The shell's meeting phase (Zoom page). Leaving releases every local key.</summary>
    public void ApplyMeeting(bool inMeeting, string? meetingLabel)
    {
        if (_inMeeting == inMeeting && _meetingLabel == meetingLabel) return;
        var wasIn = _inMeeting;
        _inMeeting = inMeeting;
        _meetingLabel = meetingLabel;
        if (wasIn && !inMeeting) ReleaseAllLocal();
        Refresh();
    }

    /// <summary>One core comms fact (on the UI thread). Null state = dropped report.</summary>
    public void ApplyComms(ZoomCommsUpdate update)
    {
        _comms = update.State;
        if (_comms is null) ReleaseAllLocal();
        Refresh();
        ReconcilePrivate();
    }

    private void Refresh()
    {
        ApplyCells();
        ApplyPeople();
        ApplyDevices();
        ApplyOps();
        ApplyMeter();
        _applying = true;
        try
        {
            foreach (var name in Refreshed) OnPropertyChanged(name);
        }
        finally { _applying = false; }
    }

    private static readonly string[] Refreshed =
    [
        nameof(InMeeting), nameof(NotInMeeting), nameof(Waiting), nameof(Unavailable), nameof(DeskVisible), nameof(UnavailableText),
        nameof(HostHintVisible), nameof(GridVisible), nameof(EditVisible), nameof(EmptyDeskVisible), nameof(EmptyDeskText),
        nameof(RailText), nameof(LampLink), nameof(LampMic), nameof(LampChannel), nameof(LampTx),
        nameof(AllHot), nameof(AllArmed), nameof(AllTone), nameof(AllText),
        nameof(SelectedMic), nameof(SelectedOutput), nameof(SidetoneOn), nameof(ToneOn),
        nameof(GainText), nameof(FramesSent), nameof(ChannelSends), nameof(Underruns), nameof(SendErrors), nameof(SendErrorsWarn),
        nameof(MicDb), nameof(MicLevelName), nameof(OpsLine), nameof(PrivateReported),
    ];

    private void RaiseDesk()
    {
        OnPropertyChanged(nameof(GridVisible));
        OnPropertyChanged(nameof(EditVisible));
        OnPropertyChanged(nameof(EmptyDeskVisible));
    }

    /// <summary>Bumped only when the cell set or a label changes (tests pin the gate).</summary>
    public int CellStructureVersion { get; private set; }

    /// <summary>Bumped only when the talent rows change (tests pin the gate).</summary>
    public int PeopleStructureVersion { get; private set; }

    // ZComms rule: a channel is a cell when someone is on it, it is named, or it is
    // keyed/latched. The collection changes only when that set (or a name) changes, and
    // then in place (ObservableCollectionSync: no Reset, surviving cells keep their
    // instance); otherwise each cell updates its properties, so a press is never destroyed
    // under the operator's finger (that eats the release).
    private void ApplyCells()
    {
        var chans = DeskVisible ? _comms!.Channels : (IReadOnlyList<ZoomCommsChannel>)[];
        var rows = chans.Where(c => c.Listeners > 0 || c.Label is not null || c.Keyed || c.Latched).ToList();
        var sig = string.Join("|", rows.Select(c => $"{c.Slot}:{c.Label}"));
        if (sig != _cellSig)
        {
            _cellSig = sig;
            CellStructureVersion++;
            ObservableCollectionSync.Apply(Cells, rows, c => c.Slot, c => c.Slot, c => new CommsCellViewModel(c.Slot), (_, _) => { });
        }
        var live = 0;
        foreach (var cell in Cells)
        {
            var ch = chans.First(c => c.Slot == cell.Slot);
            cell.Name = ch.Label ?? $"CH {ch.Slot + 1}";
            var unreachable = ch.ReachOk == 0 && ch.ReachDark > 0;
            if (ch.Room is not null || unreachable)
            {
                cell.Off = true;
                cell.Tone = CommsTone.Off;
                cell.StateText = ch.Room is string room ? $"in {room}" : "all in other rooms";
                cell.KeyHint = "";
                continue;
            }
            cell.Off = false;
            var hearing = ch.Listeners > 0;
            cell.Tone = ch.Keyed && hearing ? CommsTone.Hot
                : ch.Keyed ? CommsTone.Armed
                : hearing ? CommsTone.Ready
                : CommsTone.Idle;
            var partial = ch.ReachDark > 0 ? $" · {ch.ReachDark} elsewhere" : "";
            cell.StateText = (ch.Keyed && hearing ? (ch.Latched ? "on air · latched" : "on air")
                : ch.Keyed ? "nobody in channel"
                : !ch.Ready ? "forming…"
                : ch.Label is null ? $"{ch.Listeners} listening"
                : hearing ? "ready" : "invite in flight") + partial;
            live++;
            cell.KeyHint = live <= 9 ? live.ToString() : "";
        }
    }

    private void ApplyPeople()
    {
        var c = DeskVisible ? _comms : null;
        var roster = c?.Roster ?? [];
        var chans = c?.Channels ?? [];
        var sig = string.Join("~", roster.Select(p => $"{p.ParticipantId}:{p.Name}:{p.Talkback}:{p.Room}:{string.Join(",", p.Channels)}"))
            + "#" + string.Join("~", chans.Select(x => x.Label ?? ""));
        if (sig == _peopleSig) return;
        _peopleSig = sig;
        PeopleStructureVersion++;
        ObservableCollectionSync.Apply(People, roster, p => p.ParticipantId, p => p.ParticipantId,
            p => new CommsPersonViewModel(p.ParticipantId),
            (row, p) =>
            {
                row.Name = p.Name + (p.Room is string r ? $"  · in {r}" : "");
                row.Talkback = p.Talkback;
                row.Chips = p.Talkback ? BuildChips(p, roster, chans) : [];
            });
    }

    private IReadOnlyList<CommsChipViewModel> BuildChips(ZoomCommsPerson p, IReadOnlyList<ZoomCommsPerson> roster,
        IReadOnlyList<ZoomCommsChannel> chans)
    {
        var chips = new List<CommsChipViewModel>(chans.Count);
        foreach (var ch in chans)
        {
            var on = p.Channels.Contains(ch.Slot);
            var others = roster.Where(o => o.ParticipantId != p.ParticipantId && o.Channels.Contains(ch.Slot)).Select(o => o.Name).ToList();
            var label = ch.Label ?? $"CH {ch.Slot + 1}";
            var tip = (on ? $"Take off {label}" : $"Put on {label}")
                + (others.Count > 0 ? $", with {string.Join(", ", others)}" : on ? "" : " (empty)");
            var slot = ch.Slot;
            var pid = p.ParticipantId;
            chips.Add(new CommsChipViewModel
            {
                Slot = slot, Text = (slot + 1).ToString(), On = on, Busy = !on && others.Count > 0, Tip = tip,
                Command = new AsyncRelayCommand(() => SendAsync("comms-assign",
                    new() { ["slot"] = slot, ["participantId"] = pid, ["on"] = !on })),
            });
        }
        return chips;
    }

    // Device lists change rarely; the pickers' ItemsSource is never rebuilt per update
    // (ComboBox ItemsSource churn is a 0xc000027b trigger).
    private void ApplyDevices()
    {
        SyncStrings(Mics, _comms?.Mic.Devices ?? [], ref _micsSig);
        SyncStrings(Outputs, _comms?.Mic.SidetoneDevices ?? [], ref _outsSig);
    }

    private static void SyncStrings(ObservableCollection<string> target, IReadOnlyList<string> incoming, ref string sig)
    {
        var next = string.Join("\u001f", incoming);
        if (next == sig) return;
        sig = next;
        ObservableCollectionSync.Apply(target, incoming, s => s, s => s, s => s, (_, _) => { });
    }

    private string _opsSig = "";

    private void ApplyOps()
    {
        var log = _comms?.Log ?? [];
        var sig = string.Join("\u001f", log);
        if (sig == _opsSig) return;
        _opsSig = sig;
        // Key = text + occurrence (the log may repeat a line); newest first.
        var seen = new Dictionary<string, int>(StringComparer.Ordinal);
        var entries = new List<CommsOpsEntry>(log.Count);
        for (var i = log.Count - 1; i >= 0; i--)
        {
            var n = seen.TryGetValue(log[i], out var c) ? c + 1 : 0;
            seen[log[i]] = n;
            entries.Add(new CommsOpsEntry($"{n}\u001f{log[i]}", log[i]));
        }
        ObservableCollectionSync.Apply(OpsLines, entries, e => e.Key, e => e.Key, e => e, (_, _) => { });
    }

    // 12 segments over -48..0 dBFS (ZComms mic meter scale). Unmeasured = dark.
    private void ApplyMeter()
    {
        var lit = MicDb is double db ? (int)Math.Round(Math.Clamp((db + 48) / 48, 0, 1) * MeterSegments) : 0;
        for (var i = 0; i < Meter.Count; i++) Meter[i].Lit = i < lit;
    }

    // ------------------------------------------------------------ keys

    public Task PressAsync(CommsCellViewModel cell)
    {
        if (cell.Off || !DeskVisible) return Task.CompletedTask;
        var ch = _comms!.Channels.FirstOrDefault(c => c.Slot == cell.Slot);
        if (LatchMode) return SendAsync("comms-latch", new() { ["slot"] = cell.Slot, ["on"] = !(ch?.Latched ?? false) });
        if (cell.Held) return Task.CompletedTask;
        cell.Held = true;
        return SendAsync("comms-key", new() { ["slot"] = cell.Slot, ["on"] = true, ["leaseMs"] = LeaseMs });
    }

    public Task ReleaseAsync(CommsCellViewModel cell)
    {
        if (!cell.Held) return Task.CompletedTask;
        cell.Held = false;
        return SendAsync("comms-key", new() { ["slot"] = cell.Slot, ["on"] = false });
    }

    public Task AllPressAsync()
    {
        if (!DeskVisible) return Task.CompletedTask;
        if (LatchMode)
        {
            var allOn = _comms!.Channels.Count > 0 && _comms.Channels.All(c => c.Latched);
            return SendAsync("comms-latch", new() { ["all"] = true, ["on"] = !allOn });
        }
        if (_allHeld) return Task.CompletedTask;
        _allHeld = true;
        return SendAsync("comms-key", new() { ["all"] = true, ["on"] = true, ["leaseMs"] = LeaseMs });
    }

    public Task AllReleaseAsync()
    {
        if (!_allHeld) return Task.CompletedTask;
        _allHeld = false;
        return SendAsync("comms-key", new() { ["all"] = true, ["on"] = false });
    }

    /// <summary>True while any key (or ALL CALL) is held by this operator.</summary>
    public bool AnyHeld => _allHeld || Cells.Any(c => c.Held);

    /// <summary>Re-sends the lease on every held key (the shell's 500 ms tick).</summary>
    public Task RenewLeasesAsync()
    {
        var sends = new List<Task>();
        foreach (var c in Cells.Where(c => c.Held).ToList())
            sends.Add(SendAsync("comms-key", new() { ["slot"] = c.Slot, ["on"] = true, ["leaseMs"] = LeaseMs }));
        if (_allHeld) sends.Add(SendAsync("comms-key", new() { ["all"] = true, ["on"] = true, ["leaseMs"] = LeaseMs }));
        return Task.WhenAll(sends);
    }

    /// <summary>The shell is going away: every held key up in one line, whatever this
    /// view model believes is held (latches stay; they are deliberate).</summary>
    public Task SendReleaseAllAsync()
    {
        ReleaseAllLocal();
        return SendAsync("comms-release-all", new());
    }

    /// <summary>Digits 1-9 key the first nine reachable cells (panel-scoped).</summary>
    public CommsCellViewModel? CellForDigit(int digit)
    {
        var live = Cells.Where(c => !c.Off).ToList();
        return digit >= 1 && digit <= live.Count ? live[digit - 1] : null;
    }

    /// <summary>Focus left the page or the window, or the operator navigated away:
    /// every held key goes up.</summary>
    public async Task ReleaseAllAsync()
    {
        foreach (var c in Cells.Where(c => c.Held).ToList()) await ReleaseAsync(c);
        await AllReleaseAsync();
    }

    private void ReleaseAllLocal()
    {
        foreach (var c in Cells) c.Held = false;
        _allHeld = false;
    }

    // ------------------------------------------------------------ sends

    private Task SendGainAsync(int delta)
    {
        if (_comms is null) return Task.CompletedTask;
        var db = Math.Clamp(_comms.Mic.GainDb + delta, -20, 20);
        return db == _comms.Mic.GainDb ? Task.CompletedTask : SendAsync("comms-gain", new() { ["db"] = db });
    }

    private Task SendAsync(string verb, Dictionary<string, object?> payload)
    {
        var task = SendCoreAsync(verb, payload);
        LastSend = task;
        return task;
    }

    private async Task SendCoreAsync(string verb, Dictionary<string, object?> payload)
    {
        try
        {
            // Resume on the caller's context (the UI thread in the app) so Error is
            // raised where its bindings live.
            var result = await _transport.SendCommsAsync(verb, payload, CancellationToken.None);
            Error = result.Ok ? null : result.Error;
        }
        catch (Exception ex)
        {
            Error = ex.Message;
        }
    }
}
