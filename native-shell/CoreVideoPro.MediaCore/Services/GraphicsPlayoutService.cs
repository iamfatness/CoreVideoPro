using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>A roster slot as playout sees it: slot number, canonical source id
/// (zoom:/capture:/media:, null when unassigned) and the derived name.</summary>
public sealed record GraphicsRosterSlot(int SlotNumber, string? SourceId, string DerivedName);

/// <summary>One chip on the on-air strip. Owner is "playlist" (an instance this
/// playlist created), "auto" (targeted by a native automation rule) or "design"
/// (any other instance, e.g. a Design workspace cue).</summary>
public sealed record GraphicsStripItem(string InstanceId, string Label, string Owner, string RowId, string SourceId,
    long Revision, string Visibility, bool CuePending, bool AutoOutArmed, string RuleId = "");

/// <summary>Strip truth derived ONLY from <see cref="NativeGraphicsState.Instances"/>:
/// never from a locally accepted command. Program = ProgramRevision>0 and not
/// hidden/building-out; Preview = CueRevision>0.</summary>
public sealed record GraphicsStripState
{
    public static readonly GraphicsStripState Empty = new();
    public string CoreEpoch { get; init; } = "";
    public IReadOnlyList<GraphicsStripItem> OnProgram { get; init; } = [];
    public IReadOnlyList<GraphicsStripItem> OnPreview { get; init; } = [];
    /// <summary>Instances a native automation rule targets (the "armed rule" chips), whether or not on air.</summary>
    public IReadOnlyList<GraphicsStripItem> Auto { get; init; } = [];
    public string AutomationMode { get; init; } = "manual";
    public bool AutomationArmed { get; init; }
}

/// <summary>Playlist playout over native Graphics instances (issue #1076). Pure and
/// constructible: an invoke delegate, an observe delegate and the library. Show =
/// create/update -> cue(rideTake) -> wait for the cue revision -> trigger show from cue.
/// Preview truth and Program truth come back from native state only.</summary>
public sealed class GraphicsPlayoutService
{
    public const string InstancePrefix = "playlist-";
    public const string ClientId = "graphics-playout";
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private static readonly NativeGraphicsAnimations Motion = new() {
        In = new() { Type = "fade", DurationMs = 250, Easing = "ease-out" }, Out = new() { Type = "fade", DurationMs = 200, Easing = "ease-in" } };
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> _invoke;
    private readonly Func<NativeGraphicsState?> _observe;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly Func<DateTime> _utcNow;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly Dictionary<string, CancellationTokenSource> _autoOut = new(StringComparer.Ordinal);
    private string _epoch = "", _stripSignature = "";
    private int _pending;

    public GraphicsPlayoutService(GraphicsWorkspaceLibrary library,
        Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe,
        Func<TimeSpan, CancellationToken, Task>? delay = null, Func<DateTime>? utcNow = null)
    {
        (_library, _invoke, _observe) = (library, invoke, observe);
        _delay = delay ?? Task.Delay; _utcNow = utcNow ?? (() => DateTime.UtcNow);
        library.Changed += (_, _) => { Observe(_observe()); Changed?.Invoke(this, EventArgs.Empty); };
    }

    public event EventHandler? Changed;
    public event EventHandler? StripChanged;
    public GraphicsStripState Strip { get; private set; } = GraphicsStripState.Empty;
    public string Status { get; private set; } = "";
    public bool Busy => Volatile.Read(ref _pending) > 0;
    public IReadOnlyList<GraphicsPlaylistRow> Playlist => _library.Document.Playlist;
    public GraphicsPlaylistRow? Row(string rowId) => Playlist.FirstOrDefault(r => r.Id == rowId);
    /// <summary>The playlist row that created an instance id (prefix match), or null for a Design/automation instance.</summary>
    public GraphicsPlaylistRow? RowOf(string instanceId) => RowOf(instanceId, Playlist);
    private static GraphicsPlaylistRow? RowOf(string instanceId, IReadOnlyList<GraphicsPlaylistRow> rows) =>
        rows.FirstOrDefault(r => instanceId == InstancePrefix + r.Id || instanceId.StartsWith(InstancePrefix + r.Id + "-", StringComparison.Ordinal));
    public bool HasGuestRow => Playlist.Any(r => r.Kind == GraphicsPlaylistRow.Guest);
    public IReadOnlyList<ShotLayout> Layouts => _library.Document.ShotLayouts;
    public ShotLayout? Layout(string layoutId) => Layouts.FirstOrDefault(l => l.Id == layoutId);
    /// <summary>True when a lower-third chip is on Program: a Guest-row instance, or any other
    /// instance whose template family is lower-third. Strip truth, never local intent.</summary>
    public bool LowerThirdOnAir => IsLowerThirdOnAir(Strip, Playlist, _observe());
    public static bool IsLowerThirdOnAir(GraphicsStripState strip, IReadOnlyList<GraphicsPlaylistRow> rows, NativeGraphicsState? state) =>
        strip.OnProgram.Any(item => IsLowerThird(item, rows, state));
    private static bool IsLowerThird(GraphicsStripItem item, IReadOnlyList<GraphicsPlaylistRow> rows, NativeGraphicsState? state)
    {
        if (RowOf(item.InstanceId, rows) is { } row) return row.Kind == GraphicsPlaylistRow.Guest;
        var instance = state?.Instances.FirstOrDefault(i => i.InstanceId == item.InstanceId);
        return instance is not null && (instance.TemplateId is "compact-solid" or "minimal-accent" or "broadcast" ||
            state!.Templates.Any(t => t.TemplateId == instance.TemplateId && t.TemplateVersion == instance.TemplateVersion && t.Family == "lower-third"));
    }

    // ---- identity ---------------------------------------------------------------------
    public static string InstanceIdFor(string rowId, string sourceId) => sourceId.Length == 0 ? InstancePrefix + rowId
        : InstancePrefix + rowId + "-" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sourceId)))[..12].ToLowerInvariant();
    public static string InstanceIdFor(GraphicsPlaylistEntry entry) => InstanceIdFor(entry.RowId, entry.SourceId);
    public static bool IsOnProgram(NativeGraphicsInstance instance) => instance.ProgramRevision > 0 && instance.Visibility is not ("hidden" or "building-out");
    public static bool IsOnPreview(NativeGraphicsInstance instance) => instance.CueRevision > 0;

    // ---- entries ----------------------------------------------------------------------
    public static GraphicsPlaylistEntry EntryFor(GraphicsPlaylistRow row) => new(row.Id, "", row.Name, "");
    /// <summary>One entry per bound roster slot (Zoom, capture, media alike), in slot order, de-duplicated by source id.</summary>
    public static IReadOnlyList<GraphicsPlaylistEntry> GuestEntries(GraphicsPlaylistRow row, IEnumerable<GraphicsRosterSlot> slots, Func<string, string, (string Name, string Title)> text)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal); var entries = new List<GraphicsPlaylistEntry>();
        foreach (var slot in slots.OrderBy(s => s.SlotNumber)) {
            if (slot.SourceId is not { Length: > 0 } sourceId || !seen.Add(sourceId)) continue;
            var copy = text(sourceId, slot.DerivedName);
            entries.Add(new(row.Id, sourceId, copy.Name, copy.Title));
        }
        return entries;
    }
    /// <summary>The <c>variant:&lt;id&gt;</c> source id behind one of a names-list row's
    /// instances, or "" when the instance is not one of its lines.</summary>
    public static string VariantSourceIdOf(GraphicsPlaylistRow row, string instanceId) =>
        row.Variants.Select(v => GraphicsPlaylistEntry.VariantPrefix + v.Id).FirstOrDefault(s => InstanceIdFor(row.Id, s) == instanceId) ?? "";
    /// <summary>One entry per names-list line, in list order.</summary>
    public static IReadOnlyList<GraphicsPlaylistEntry> VariantEntries(GraphicsPlaylistRow row) =>
        row.Variants.Select(v => new GraphicsPlaylistEntry(row.Id, GraphicsPlaylistEntry.VariantPrefix + v.Id, v.Name, v.Title)).ToArray();
    /// <summary>The variant a bare hotkey / rowId-only Show plays: the one after whichever
    /// variant of this row is on Program (wrapping), else the first. Null when the row has none.</summary>
    public static GraphicsPlaylistEntry? NextVariant(GraphicsPlaylistRow row, IEnumerable<string> onProgramSourceIds)
    {
        var entries = VariantEntries(row); if (entries.Count == 0) return null;
        var onAir = onProgramSourceIds.ToHashSet(StringComparer.Ordinal);
        var current = -1; for (var i = 0; i < entries.Count; i++) if (onAir.Contains(entries[i].SourceId)) current = i;
        return entries[(current + 1) % entries.Count];
    }
    /// <summary>Every multi-entry row expanded: Guest rows per bound roster slot, names-list rows
    /// per variant. (Historical name; callers index it by SourceId either way.)</summary>
    public IReadOnlyList<GraphicsPlaylistEntry> GuestEntries(IEnumerable<GraphicsRosterSlot> slots, Func<string, string, (string Name, string Title)> text)
    {
        var list = slots.ToArray();
        return Playlist.SelectMany(r => r.HasVariants ? VariantEntries(r) : r.Kind == GraphicsPlaylistRow.Guest ? GuestEntries(r, list, text) : []).ToArray();
    }
    public GraphicsPlaylistEntry? GuestEntryFor(string sourceId, IEnumerable<GraphicsRosterSlot> slots, Func<string, string, (string Name, string Title)> text) =>
        GuestEntries(slots, text).FirstOrDefault(e => e.SourceId == sourceId);

    // ---- legacy lower-third intent (Stage C1) --------------------------------------------
    /// <summary>The legacy "LT in/out" intent (footer button, graphics.lowerThird.toggle/set)
    /// mapped onto the Guest row: <paramref name="subject"/> is the guest entry of the current
    /// Program subject (null when none is bound). In = Show unless already on air; Out = Play
    /// Out every Guest-row instance on Program (the subject may have changed since). Lower-third
    /// authority is adopted through <paramref name="adoptInstancesAuthority"/> only after the
    /// first ACCEPTED Show: flipping earlier would erase the legacy overlay layers with
    /// nothing to replace them.</summary>
    public async Task<NativeGraphicsOperationResult> ApplyLowerThirdIntentAsync(bool visible, GraphicsPlaylistEntry? subject, bool authorityIsInstances,
        Func<CancellationToken, Task<bool>> adoptInstancesAuthority, CancellationToken cancellation = default)
    {
        if (!visible) {
            var targets = (_observe()?.Instances ?? []).Where(i => IsOnProgram(i) && RowOf(i.InstanceId)?.Kind == GraphicsPlaylistRow.Guest).Select(i => i.InstanceId).ToArray();
            if (targets.Length == 0) { Observe(_observe()); return new() { Accepted = true, Reason = "No playlist lower third is on Program." }; }
            NativeGraphicsOperationResult result = new() { Accepted = true };
            foreach (var instanceId in targets) {
                result = await PlayOutAsync(instanceId, cancellation);
                if (!result.Accepted) return result;
            }
            return result with { Reason = "Lower third hidden." };
        }
        if (subject is null) return Refuse("Lower third needs a Program subject bound to a Show Input. Choose a guest in the playlist instead.");
        var id = InstanceIdFor(subject);
        if (_observe()?.Instances.FirstOrDefault(i => i.InstanceId == id) is { } current && IsOnProgram(current))
            return new() { Accepted = true, InstanceId = id, CurrentRevision = current.Revision, Reason = "The lower third is already on air." };
        var played = await PlayInAsync(subject, cancellation);
        if (!played.Accepted || authorityIsInstances) return played;
        var adopted = await adoptInstancesAuthority(cancellation);
        return played with { Reason = adopted ? "Shown; lower thirds now follow the Graphics playlist." : played.Reason + " Lower-third authority is still legacy; retry to adopt it." };
    }

    // ---- shot layouts (slice 2) -----------------------------------------------------------
    /// <summary>Cue a shot layout: select its scene to Preview through <paramref name="sceneSelect"/>
    /// (the ViewModel's scene cue path; false = the scene is not in this show and the layout is
    /// refused before anything is sent), then Cue every row, so one Take carries the picture
    /// and the graphics. A Guest row cues <paramref name="sceneGuests"/> (entries bound to slots
    /// routed in that scene) when any were resolved, otherwise <paramref name="programSubject"/>.
    /// <see cref="ShotLayout.PlayOutOthers"/> plays out every Program graphic the layout does not
    /// carry. Rows that no longer exist refuse the whole layout; nothing is half-applied.</summary>
    public Task<NativeGraphicsOperationResult> CueLayoutAsync(string layoutId, Func<string, CancellationToken, Task<bool>> sceneSelect,
        GraphicsPlaylistEntry? programSubject, IReadOnlyList<GraphicsPlaylistEntry>? sceneGuests = null, CancellationToken cancellation = default) =>
        Run(() => CueLayout(layoutId, sceneSelect, programSubject, sceneGuests, cancellation));

    /// <summary>CueLayoutAsync, then the ordinary Take through <paramref name="take"/> (null = taken,
    /// otherwise the refusal). A layout with no scene and no rows (Clean) has nothing to Take.</summary>
    public async Task<NativeGraphicsOperationResult> CueAndTakeLayoutAsync(string layoutId, Func<string, CancellationToken, Task<bool>> sceneSelect,
        GraphicsPlaylistEntry? programSubject, IReadOnlyList<GraphicsPlaylistEntry>? sceneGuests, Func<CancellationToken, Task<string?>> take, CancellationToken cancellation = default)
    {
        var cued = await CueLayoutAsync(layoutId, sceneSelect, programSubject, sceneGuests, cancellation);
        if (!cued.Accepted) return cued;
        var layout = Layout(layoutId);
        if (layout is null || (layout.SceneId.Length == 0 && layout.RowIds.Count == 0)) return cued with { Reason = cued.Reason + " Nothing to Take." };
        var error = await take(cancellation);
        return error is null ? cued with { Reason = $"{layout.Name} taken." } : Refuse($"{layout.Name} is cued on Preview but the Take was refused: {error}");
    }

    private async Task<NativeGraphicsOperationResult> CueLayout(string layoutId, Func<string, CancellationToken, Task<bool>> sceneSelect,
        GraphicsPlaylistEntry? programSubject, IReadOnlyList<GraphicsPlaylistEntry>? sceneGuests, CancellationToken cancellation)
    {
        await _library.LoadAsync();
        if (Layout(layoutId) is not { } layout) return Refuse(layoutId.Length == 0 ? "Choose a shot layout." : "That shot layout no longer exists.");
        // Resolve everything before the first side effect so a broken layout is refused whole.
        var entries = new List<GraphicsPlaylistEntry>();
        foreach (var rowId in layout.RowIds) {
            if (Row(rowId) is not { } row) return Refuse($"{layout.Name}: playlist row '{rowId}' no longer exists. Edit the layout.");
            if (row.Kind != GraphicsPlaylistRow.Guest) { entries.Add(EntryFor(row)); continue; }
            var guests = (sceneGuests ?? []).Where(e => e.RowId == row.Id).ToArray();
            if (guests.Length > 0) entries.AddRange(guests);
            else if (programSubject is not null) entries.Add(programSubject with { RowId = row.Id });
            else return Refuse($"{layout.Name}: {row.Name} needs a Program subject bound to a Show Input. Route a guest first.");
        }
        if (layout.SceneId.Length > 0 && !await sceneSelect(layout.SceneId, cancellation))
            return Refuse($"{layout.Name}: scene '{layout.SceneId}' is not in this show, so the layout was not cued. Edit the layout to pick a scene.");
        var carried = new HashSet<string>(StringComparer.Ordinal);
        foreach (var entry in entries) {
            var cued = await Cue(entry, cancellation);
            if (!cued.Accepted) return cued with { Reason = $"{layout.Name}: {cued.Reason}" };
            carried.Add(cued.InstanceId);
        }
        if (layout.PlayOutOthers) {
            var state = Available();
            foreach (var instance in state.Instances.Where(i => (IsOnProgram(i) || i.ShowPending) && !carried.Contains(i.InstanceId)).ToArray()) {
                var hidden = await Hide(instance.InstanceId, cancellation);
                if (!hidden.Accepted) return hidden with { Reason = $"{layout.Name}: {hidden.Reason}" };
            }
        }
        var what = layout.SceneId.Length > 0 && entries.Count > 0 ? $"scene {layout.SceneId} and {Plural(entries.Count)} on Preview; Take carries both."
            : layout.SceneId.Length > 0 ? $"scene {layout.SceneId} on Preview." : entries.Count > 0 ? $"{Plural(entries.Count)} on Preview; they ride the next Take." : "Program graphics hidden.";
        return new() { Accepted = true, Reason = $"{layout.Name} cued: {what}" };
        static string Plural(int count) => count == 1 ? "1 graphic" : count + " graphics";
    }

    // ---- strip ------------------------------------------------------------------------
    public void Observe(NativeGraphicsState? state)
    {
        if (state is null) return;
        if (Strip.CoreEpoch.Length > 0 && Strip.CoreEpoch != state.CoreEpoch) CancelAllAutoOut();
        var rows = Playlist; var rules = state.Automation?.Configuration.Rules ?? [];
        HashSet<string> armed; lock (_autoOut) armed = [.. _autoOut.Keys];
        GraphicsStripItem Item(NativeGraphicsInstance i)
        {
            var row = RowOf(i.InstanceId, rows);
            var rule = rules.FirstOrDefault(r => r.Enabled && r.InstanceId == i.InstanceId);
            var owner = row is not null ? "playlist" : rule is not null ? "auto" : "design";
            var source = i.Binding?.SourceId ?? i.SourceId;
            // A names-list instance has no binding; its line is recoverable from the instance id.
            if (source.Length == 0 && row is { HasVariants: true }) source = VariantSourceIdOf(row, i.InstanceId);
            var label = row is null ? (i.Label.Length > 0 ? i.Label : i.Fields.Name.Length > 0 ? i.Fields.Name : i.TemplateId)
                : (row.Kind == GraphicsPlaylistRow.Guest || row.HasVariants) && i.Fields.Name.Length > 0 ? row.Name + " · " + i.Fields.Name : row.Name;
            return new(i.InstanceId, label, owner, row?.Id ?? "", source, i.Revision, i.Visibility, i.CuePending, armed.Contains(i.InstanceId), rule?.Id ?? "");
        }
        var next = new GraphicsStripState {
            CoreEpoch = state.CoreEpoch,
            OnProgram = state.Instances.Where(IsOnProgram).Select(Item).ToArray(),
            OnPreview = state.Instances.Where(IsOnPreview).Select(Item).ToArray(),
            Auto = state.Instances.Where(i => !i.InstanceId.StartsWith(InstancePrefix, StringComparison.Ordinal) && rules.Any(r => r.Enabled && r.InstanceId == i.InstanceId)).Select(Item).ToArray(),
            AutomationMode = state.Automation?.Mode ?? "manual", AutomationArmed = state.Automation?.Armed == true
        };
        var signature = next.CoreEpoch + "|" + next.AutomationMode + ":" + next.AutomationArmed + "|" +
            string.Join('|', next.OnProgram.Concat(next.OnPreview).Concat(next.Auto).Select(i => $"{i.InstanceId}:{i.Label}:{i.Owner}:{i.Revision}:{i.Visibility}:{i.CuePending}:{i.AutoOutArmed}:{i.RuleId}")) +
            "|" + next.OnProgram.Count + ":" + next.OnPreview.Count + ":" + next.Auto.Count;
        if (signature == _stripSignature) return;
        _stripSignature = signature; Strip = next; StripChanged?.Invoke(this, EventArgs.Empty);
    }

    // ---- operations -------------------------------------------------------------------
    public Task<NativeGraphicsOperationResult> EnsureInstanceAsync(GraphicsPlaylistEntry entry, CancellationToken cancellation = default) => Run(() => Ensure(entry, cancellation));
    public Task<NativeGraphicsOperationResult> ToPreviewAsync(GraphicsPlaylistEntry entry, CancellationToken cancellation = default) => Run(() => Cue(entry, cancellation));
    public Task<NativeGraphicsOperationResult> PlayInAsync(GraphicsPlaylistEntry entry, CancellationToken cancellation = default) => Run(async () => {
        var cued = await Cue(entry, cancellation);
        if (!cued.Accepted) return cued;
        // A names list is one lower third with many lines: showing a line replaces the row's
        // other lines on Program (the operator asked for the next name, not a stack of them).
        if (entry.IsVariant && Row(entry.RowId) is { HasVariants: true } list)
            foreach (var sibling in Strip.OnProgram.Where(i => i.RowId == list.Id && i.InstanceId != cued.InstanceId).ToArray()) {
                var hidden = await Hide(sibling.InstanceId, cancellation);
                if (!hidden.Accepted) return hidden with { Reason = "Could not take the previous name off: " + hidden.Reason };
            }
        var shown = await Send("graphics.instance.trigger", new() { ["instanceId"] = cued.InstanceId, ["expectedRevision"] = cued.CurrentRevision, ["target"] = "cue", ["action"] = "show" }, cancellation);
        if (shown.Accepted && Row(entry.RowId) is { } row) ArmAutoOut(row, cued.InstanceId, _epoch);
        return shown.Accepted ? shown with { InstanceId = cued.InstanceId, Reason = "Shown from Preview." } : shown;
    });
    public Task<NativeGraphicsOperationResult> PlayOutAsync(GraphicsPlaylistEntry entry, CancellationToken cancellation = default) => PlayOutAsync(InstanceIdFor(entry), cancellation);
    public Task<NativeGraphicsOperationResult> PlayOutAsync(string instanceId, CancellationToken cancellation = default) => Run(() => Hide(instanceId, cancellation));
    public Task<NativeGraphicsOperationResult> RemoveFromPreviewAsync(string instanceId, CancellationToken cancellation = default) => Run(() => Uncue(instanceId, cancellation));
    public Task<NativeGraphicsOperationResult> PlayOutAllAsync(CancellationToken cancellation = default) => Run(async () => {
        var state = Available(); NativeGraphicsOperationResult result = new() { Accepted = true, Reason = "Nothing is on Program." };
        foreach (var instance in state.Instances.Where(i => IsOnProgram(i) || i.ShowPending).ToArray()) {
            result = await Hide(instance.InstanceId, cancellation);
            if (!result.Accepted) return result;
        }
        return result.Accepted ? result with { Reason = "All graphics hidden." } : result;
    });
    public Task<NativeGraphicsOperationResult> ClearPreviewAsync(CancellationToken cancellation = default) => Run(async () => {
        var state = Available(); NativeGraphicsOperationResult result = new() { Accepted = true, Reason = "Preview is already clear." };
        foreach (var instance in state.Instances.Where(IsOnPreview).ToArray()) {
            result = await Uncue(instance.InstanceId, cancellation);
            if (!result.Accepted) return result;
        }
        return result.Accepted ? result with { Reason = "Preview cleared." } : result;
    });

    private async Task<NativeGraphicsOperationResult> Ensure(GraphicsPlaylistEntry entry, CancellationToken cancellation)
    {
        await _library.LoadAsync();
        if (Row(entry.RowId) is not { } row) return Refuse("That playlist row no longer exists.");
        var state = Available(); var epoch = _epoch = state.CoreEpoch;
        var templateId = row.Image?.TemplateId ?? row.TemplateId; var version = row.Image?.TemplateVersion ?? row.TemplateVersion;
        if (templateId.Length == 0) return Refuse($"{row.Name} has no graphic yet. Choose an image or template for it first.");
        var pin = _library.Document.Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == version);
        var installed = state.Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == version);
        if (installed is null && pin is not null) {
            var imported = await Send("graphics.template.install", new() { ["manifest"] = pin.Definition.GetRawText(), ["packageRoot"] = pin.PackageRoot }, cancellation);
            if (!imported.Accepted) return imported;
            var limit = _utcNow().AddSeconds(5);
            do {
                await _delay(TimeSpan.FromMilliseconds(50), cancellation);
                var listed = await Send("graphics.template.list", [], cancellation);
                if (!listed.Accepted) return listed;
                installed = Current(epoch).Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == version);
            } while (installed is null && _utcNow() < limit);
        }
        if (installed is null) return Refuse($"{row.Name}: template {templateId} v{version} is not installed in this core. Open it in Design to import it.");
        if (pin is not null && pin.ManifestHash != installed.ManifestHash) return Refuse($"{row.Name}: the installed template differs from its saved pin. Program is unchanged.");
        state = Current(epoch);
        var declarative = installed.Runtime == "declarative";
        var values = row.Fields.ToDictionary(p => p.Key, p => p.Value.Clone(), StringComparer.Ordinal);
        // Bindings are template variables: apply each one the template declares (compat
        // lower thirds declare name/title implicitly); unknown variables are ignored.
        if (entry.IsVariant || (row.FollowSources && entry.SourceId.Length > 0))
            foreach (var binding in entry.Bindings)
                if (declarative ? installed.Fields.Any(f => f.Id == binding.Key && f.Type == "string") : binding.Key is "name" or "title")
                    values[binding.Key] = JsonSerializer.SerializeToElement(binding.Value);
        if (declarative) foreach (var key in values.Keys.Where(k => installed.Fields.All(f => f.Id != k)).ToArray()) values.Remove(key);
        else foreach (var key in values.Keys.Where(k => k is not ("name" or "title")).ToArray()) values.Remove(key);
        var identity = entry.SourceId.Length == 0 ? null : state.Sources.FirstOrDefault(s => s.VideoOn && s.Identity?.SourceId == entry.SourceId)?.Identity;
        var fields = JsonSerializer.SerializeToElement(values, Json);
        var id = InstanceIdFor(entry);
        var motion = row.Motion ?? Motion; var placement = row.Placement ?? installed.DefaultPlacement;
        var draft = new NativeGraphicsDraft(templateId, version, fields, row.Style, motion, placement, Image: row.Image);
        if (state.Instances.FirstOrDefault(i => i.InstanceId == id) is not { } existing) {
            var body = new Dictionary<string, object?> {
                ["instanceId"] = id, ["templateId"] = templateId, ["templateVersion"] = version, ["scope"] = "global", ["sceneId"] = "",
                ["fields"] = fields, ["style"] = draft.StyleWire(), ["animations"] = motion, ["placement"] = placement, ["image"] = row.Image };
            if (identity is not null) body["binding"] = identity;
            var created = await Send("graphics.instance.create", body, cancellation);
            if (!created.Accepted) return created;
            if (created.CurrentRevision < 1 || created.InstanceId != id) return Refuse("The native create result has no matching revision identity.");
            await ConfigureClock(row, id, expectedClockRevision: 0, requested: null, cancellation);
            return created with { Reason = "Ready." };
        }
        await ConfigureClock(row, id, existing.ClockConfigurationRevision, existing.ClockRequestedConfiguration, cancellation);
        // Only the cue copy is refreshed; a Program copy changes on the next Show, never here.
        if (SameFields(existing.Fields, values) && (identity is null || existing.Binding == identity))
            return new() { Accepted = true, InstanceId = id, CurrentRevision = existing.Revision, Reason = "Ready." };
        var update = new Dictionary<string, object?> { ["instanceId"] = id, ["expectedRevision"] = existing.Revision, ["target"] = "cue", ["fields"] = fields };
        if (identity is not null) update["binding"] = identity;
        var updated = await Send("graphics.instance.update", update, cancellation);
        if (!updated.Accepted) return updated;
        var revision = updated.CurrentRevision > 0 ? updated.CurrentRevision : Current(epoch).Instances.FirstOrDefault(i => i.InstanceId == id)?.Revision ?? 0;
        return revision < 1 ? Refuse("The native update result has no revision.") : updated with { InstanceId = id, CurrentRevision = revision, Reason = "Ready." };
    }

    /// <summary>Apply the row's clock settings to its instance when they differ from what the
    /// core already has. Non-fatal: a template without a clock answers instance-has-no-clock and
    /// the graphic still plays.</summary>
    private async Task ConfigureClock(GraphicsPlaylistRow row, string instanceId, long expectedClockRevision, NativeGraphicsClockConfiguration? requested, CancellationToken cancellation)
    {
        if (row.Clock is not { IsValid: true } clock) return;
        if (requested is not null && SameClock(requested, clock)) return;
        await Send("graphics.clock.control", new() { ["instanceId"] = instanceId, ["action"] = "configure", ["configuration"] = clock, ["expectedClockRevision"] = expectedClockRevision }, cancellation);
    }
    private static bool SameClock(NativeGraphicsClockConfiguration a, NativeGraphicsClockConfiguration b) =>
        a.Mode == b.Mode && a.Zone == b.Zone && a.Format == b.Format && a.Finish == b.Finish && a.OffsetMinutes == b.OffsetMinutes && a.DurationSeconds == b.DurationSeconds;

    /// <summary>Countdown transport for a playlist row's instance: start | pause | resume | reset.
    /// Never shows or hides the graphic. Refused when the row has no instance yet.</summary>
    public Task<NativeGraphicsOperationResult> ClockAsync(GraphicsPlaylistEntry entry, string action, CancellationToken cancellation = default) => Run(async () => {
        if (action is not ("start" or "pause" or "resume" or "reset")) return Refuse("Clock action must be start, pause, resume or reset.");
        var id = InstanceIdFor(entry);
        if (_observe()?.Instances.All(i => i.InstanceId != id) != false) return Refuse("That row is not on Preview or Program yet. Cue or Show it first.");
        var result = await Send("graphics.clock.control", new() { ["instanceId"] = id, ["action"] = action }, cancellation);
        return result.Accepted ? result with { InstanceId = id, Reason = "Clock " + action + "." } : result;
    });

    private async Task<NativeGraphicsOperationResult> Cue(GraphicsPlaylistEntry entry, CancellationToken cancellation)
    {
        var ensured = await Ensure(entry, cancellation);
        if (!ensured.Accepted) return ensured;
        var id = ensured.InstanceId; var revision = ensured.CurrentRevision; var epoch = _epoch;
        var cued = await Send("graphics.instance.cue", new() { ["instanceId"] = id, ["expectedRevision"] = revision, ["rideTake"] = true }, cancellation);
        if (!cued.Accepted) return cued;
        var until = _utcNow().AddSeconds(5);
        while (true) {
            var current = Current(epoch).Instances.FirstOrDefault(i => i.InstanceId == id);
            if (current is null || current.Revision != revision) return Refuse("The graphic changed while cueing. Retry explicitly.");
            if (current.CueRevision == revision && !current.CuePending) break;
            if (_utcNow() >= until) return Refuse("The graphic is still preparing. Retry when ready.");
            await _delay(TimeSpan.FromMilliseconds(50), cancellation);
        }
        return new() { Accepted = true, InstanceId = id, CurrentRevision = revision, Reason = "Cued to Preview; it rides the next Take." };
    }

    private async Task<NativeGraphicsOperationResult> Hide(string instanceId, CancellationToken cancellation)
    {
        CancelAutoOut(instanceId);
        var state = Available(); _epoch = state.CoreEpoch;
        if (state.Instances.FirstOrDefault(i => i.InstanceId == instanceId) is not { } instance) return Refuse("That graphic no longer exists.");
        if (!(instance.ProgramRevision > 0 || instance.ShowPending)) return Refuse("That graphic is not on Program.");
        return await Send("graphics.instance.trigger", new() { ["instanceId"] = instanceId, ["expectedRevision"] = instance.Revision, ["action"] = "hide" }, cancellation);
    }

    private async Task<NativeGraphicsOperationResult> Uncue(string instanceId, CancellationToken cancellation)
    {
        var state = Available(); _epoch = state.CoreEpoch;
        if (state.Instances.FirstOrDefault(i => i.InstanceId == instanceId) is not { } instance) return Refuse("That graphic no longer exists.");
        if (!IsOnPreview(instance)) return Refuse("That graphic is not on Preview.");
        return await Send("graphics.instance.trigger", new() { ["instanceId"] = instanceId, ["expectedRevision"] = instance.Revision, ["action"] = "uncue" }, cancellation);
    }

    // ---- auto-out ---------------------------------------------------------------------
    private void ArmAutoOut(GraphicsPlaylistRow row, string instanceId, string epoch)
    {
        CancelAutoOut(instanceId);
        if (row.PlayOutAfterMs <= 0) return;
        var source = new CancellationTokenSource();
        lock (_autoOut) _autoOut[instanceId] = source;
        _ = AutoOutAsync(instanceId, row.PlayOutAfterMs, epoch, source);
    }
    private async Task AutoOutAsync(string instanceId, int afterMs, string epoch, CancellationTokenSource source)
    {
        try { await _delay(TimeSpan.FromMilliseconds(afterMs), source.Token); }
        catch (OperationCanceledException) { return; }
        lock (_autoOut) { if (!_autoOut.TryGetValue(instanceId, out var current) || !ReferenceEquals(current, source)) return; _autoOut.Remove(instanceId); }
        source.Dispose();
        if (_observe() is not { } state || state.CoreEpoch != epoch) return;
        if (state.Instances.FirstOrDefault(i => i.InstanceId == instanceId) is not { } instance || !(instance.ProgramRevision > 0 || instance.ShowPending)) { Observe(state); return; }
        await Run(() => Hide(instanceId, CancellationToken.None));
    }
    private void CancelAutoOut(string instanceId)
    {
        CancellationTokenSource? source;
        lock (_autoOut) { if (_autoOut.Remove(instanceId, out source) is false) return; }
        source!.Cancel(); source.Dispose();
    }
    private void CancelAllAutoOut()
    {
        CancellationTokenSource[] sources;
        lock (_autoOut) { sources = [.. _autoOut.Values]; _autoOut.Clear(); }
        foreach (var source in sources) { source.Cancel(); source.Dispose(); }
    }

    // ---- plumbing ---------------------------------------------------------------------
    private async Task<NativeGraphicsOperationResult> Run(Func<Task<NativeGraphicsOperationResult>> action)
    {
        Interlocked.Increment(ref _pending); Changed?.Invoke(this, EventArgs.Empty);
        await _gate.WaitAsync();
        NativeGraphicsOperationResult result;
        try { result = await action(); }
        catch (InvalidOperationException ex) { result = Refuse(ex.Message); }
        catch (OperationCanceledException) { result = Refuse("Graphics playout cancelled. Retry explicitly when ready."); }
        finally { _gate.Release(); Interlocked.Decrement(ref _pending); }
        Status = result.Reason; Observe(_observe()); Changed?.Invoke(this, EventArgs.Empty);
        return result;
    }
    private NativeGraphicsState Available() => _observe() is { } state && state.CoreEpoch.Length > 0 ? state : throw new InvalidOperationException("Native Graphics is unavailable.");
    private NativeGraphicsState Current(string epoch) => _observe() is { } state && state.CoreEpoch == epoch
        ? state : throw new InvalidOperationException("Native core changed. Graphics playout cancelled; no automatic replay.");
    private Task<NativeGraphicsOperationResult> Send(string action, Dictionary<string, object?> body, CancellationToken cancellation)
    {
        var epoch = _epoch.Length > 0 ? _epoch : Available().CoreEpoch; Current(epoch);
        body["schemaVersion"] = 1; body["clientId"] = ClientId; body["requestId"] = "playout-" + Guid.NewGuid().ToString("N"); body["coreEpoch"] = epoch;
        return _invoke(action, JsonSerializer.Serialize(body, Json), cancellation);
    }
    private static bool SameFields(NativeGraphicsFields existing, IReadOnlyDictionary<string, JsonElement> values)
    {
        var current = JsonSerializer.SerializeToElement(existing, Json).EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
        return values.All(pair => current.TryGetValue(pair.Key, out var value) && JsonElement.DeepEquals(value, pair.Value));
    }
    private static NativeGraphicsOperationResult Refuse(string reason) => new() { Reason = reason };
}
