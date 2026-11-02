using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Outcome of a design lifecycle operation. <see cref="Template"/> is the installed
/// native template when the operation installed or found one.</summary>
public sealed record GraphicsDesignResult(bool Ok, string Message, GraphicsDesign? Design, NativeGraphicsTemplate? Template);

/// <summary>Design lifecycle (Graphics Designer slice 1): create blank or from a look, open, save as
/// a new version, duplicate, delete, and install on demand. Designs live in the show document; the
/// native registry holds only the versions something needs. Installs go through the same
/// install-then-poll path as playout's pin reinstall (<see cref="GraphicsTemplateInstall"/>).
/// "Referenced" = pinned by a playlist row or saved cue, or used by a live instance. Only a
/// playlist-row pin blocks Delete; any reference keeps that version installed.</summary>
public sealed class GraphicsDesignService
{
    public const string ClientId = "graphics-designs";
    public const string LibraryFull = GraphicsTemplateInstall.LibraryFull;
    private const string NameRule = "Use a design name of 1–80 characters.";
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> _invoke;
    private readonly Func<NativeGraphicsState?> _observe;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly Func<DateTime> _utcNow;
    private readonly Func<string> _hex6;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private string _epoch = "";

    public GraphicsDesignService(GraphicsWorkspaceLibrary library,
        Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe,
        Func<TimeSpan, CancellationToken, Task>? delay = null, Func<DateTime>? utcNow = null, Func<string>? hex6 = null)
    {
        (_library, _invoke, _observe) = (library, invoke, observe);
        _delay = delay ?? Task.Delay; _utcNow = utcNow ?? (() => DateTime.UtcNow);
        _hex6 = hex6 ?? (() => RandomNumberGenerator.GetHexString(6, lowercase: true));
    }

    /// <summary>The last operation's message (also set by <see cref="EnsureInstalledForPlaylistAsync"/>).</summary>
    public string Status { get; private set; } = "";
    public IReadOnlyList<GraphicsDesign> Designs => _library.Document.Designs;

    public Task<GraphicsDesignResult> CreateBlankAsync(string kind, string name, CancellationToken ct = default) => Run(async () => {
        if (!GraphicsDesignSeeds.Kinds.Contains(kind)) return Fail("Choose a lower third, title, bug or full screen to start from.");
        if (CleanName(name) is not { } clean) return Fail(NameRule);
        if (Full()) return Fail(TooMany);
        var templateId = NewTemplateId(clean);
        return await Create(GraphicsDesignSeeds.Manifest(kind, templateId, 1, clean), "", clean, ct);
    });

    /// <summary>Start from a look: a design or any installed declarative template. Always a copy under
    /// a new design id; the source (built-ins above all) is never edited in place.</summary>
    public Task<GraphicsDesignResult> CreateFromAsync(string templateId, int templateVersion, string name, CancellationToken ct = default) => Run(async () => {
        if (CleanName(name) is not { } clean) return Fail(NameRule);
        if (Full()) return Fail(TooMany);
        JsonElement definition; var packageRoot = "";
        if (_library.Document.Designs.FirstOrDefault(d => d.TemplateId == templateId && d.TemplateVersion == templateVersion) is { } design) definition = design.Definition;
        else if (Available().Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == templateVersion) is { Definition: { } installed } look) {
            if (look.Runtime != "declarative") return Fail("This look cannot be copied into a design.");
            (definition, packageRoot) = (installed, look.PackageRoot);
        }
        else if (_library.Document.Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == templateVersion) is { } pin) (definition, packageRoot) = (pin.Definition, pin.PackageRoot);
        else return Fail("That look is not available. Open it once in Design, then try again.");
        var copy = Stamp(definition, NewTemplateId(clean), 1, clean);
        return await Create(copy, packageRoot, clean, ct);
    });

    /// <summary>Install the design's current version when the core does not have it.</summary>
    public Task<GraphicsDesignResult> OpenAsync(string designId, CancellationToken ct = default) => Run(async () => {
        if (Find(designId) is not { } design) return Fail(Missing);
        var (template, refusal) = await Install(design.Definition, "", design.TemplateId, design.TemplateVersion, ct);
        return template is null ? Fail(refusal) : new(true, Status = "Design ready.", design, template);
    });

    /// <summary>Install the edited manifest as the next version of the design's own template id,
    /// store it as the design's definition, then remove superseded versions nothing references.</summary>
    public Task<GraphicsDesignResult> SaveAsync(string designId, NativeGraphicsScratch scratch, CancellationToken ct = default) => Run(async () => {
        if (Find(designId) is not { } design) return Fail(Missing);
        if (scratch is null || JsonNode.Parse(scratch.Manifest) is not JsonObject manifest) return Fail("The design is not a valid template.");
        var state = Available();
        var version = checked(state.Templates.Where(t => t.TemplateId == design.TemplateId).Select(t => t.TemplateVersion)
            .Concat(_library.Document.Templates.Where(t => t.TemplateId == design.TemplateId).Select(t => t.TemplateVersion))
            .Append(design.TemplateVersion).Max() + 1);
        manifest["templateId"] = design.TemplateId; manifest["templateVersion"] = version;
        var packageRoot = state.Templates.FirstOrDefault(t => t.TemplateId == design.TemplateId && t.TemplateVersion == design.TemplateVersion)?.PackageRoot ?? "";
        var (template, refusal) = await Install(JsonSerializer.SerializeToElement(manifest), packageRoot, design.TemplateId, version, ct);
        if (template is null) return Fail(refusal);
        var saved = design with { TemplateVersion = version, Definition = (template.Definition ?? JsonSerializer.SerializeToElement(manifest)).Clone(),
            Family = template.Family.Length > 0 ? template.Family : design.Family, UpdatedAt = _utcNow() };
        if (!await Store(_library.Document.Designs.Select(d => d.DesignId == design.DesignId ? saved : d).ToArray(), $"Saved {saved.Name} as version {version}.")) {
            await Remove(design.TemplateId, version, ct);
            return Fail(_library.Status);
        }
        await RemoveUnreferenced(design.TemplateId, keep: version, ct);
        return new(true, Status = _library.Status, saved, template);
    });

    /// <summary>A new design (new id) with the same definition. Not installed until opened.</summary>
    public Task<GraphicsDesignResult> DuplicateAsync(string designId, string name, CancellationToken ct = default) => Run(async () => {
        if (Find(designId) is not { } design) return Fail(Missing);
        if (CleanName(name) is not { } clean) return Fail(NameRule);
        if (Full()) return Fail(TooMany);
        var templateId = NewTemplateId(clean);
        var copy = design with { DesignId = templateId, Name = clean, TemplateId = templateId, TemplateVersion = 1,
            Definition = JsonSerializer.SerializeToElement(Stamp(design.Definition, templateId, 1, clean)), UpdatedAt = _utcNow() };
        return await Store([.. _library.Document.Designs, copy], $"Duplicated {design.Name} as {clean}.") ? new(true, Status = _library.Status, copy, null) : Fail(_library.Status);
    });

    /// <summary>Refused while a playlist row pins the design (the message names the rows); otherwise
    /// removes the design and every installed version no saved cue or live instance still uses.</summary>
    public Task<GraphicsDesignResult> DeleteAsync(string designId, CancellationToken ct = default) => Run(async () => {
        if (Find(designId) is not { } design) return Fail(Missing);
        var rows = _library.Document.Playlist.Where(r => r.TemplateId == design.TemplateId || r.Image?.TemplateId == design.TemplateId).Select(r => r.Name).Distinct().ToArray();
        if (rows.Length > 0) return Fail($"This design is in the playlist. Used by: {string.Join(", ", rows)}.");
        if (!await Store(_library.Document.Designs.Where(d => d.DesignId != design.DesignId).ToArray(), $"Deleted {design.Name}.")) return Fail(_library.Status);
        await RemoveUnreferenced(design.TemplateId, keep: null, ct);
        return new(true, Status = _library.Status, design, null);
    });

    /// <summary>After a core restart: install the design versions playlist rows and saved cues pin.
    /// Designs nothing pins stay uninstalled until opened. Failures land in <see cref="Status"/>.</summary>
    public async Task EnsureInstalledForPlaylistAsync(CancellationToken ct = default) => await Run(async () => {
        var document = _library.Document; var problems = new List<string>();
        foreach (var (id, version) in Pins(document, rowsAndCuesOnly: true).Where(p => GraphicsDesignIds.IsDesignTemplateId(p.Id))) {
            JsonElement definition; var packageRoot = "";
            if (document.Templates.FirstOrDefault(t => t.TemplateId == id && t.TemplateVersion == version) is { } pin) (definition, packageRoot) = (pin.Definition, pin.PackageRoot);
            else if (document.Designs.FirstOrDefault(d => d.TemplateId == id && d.TemplateVersion == version) is { } design) definition = design.Definition;
            else continue;
            var (template, refusal) = await Install(definition, packageRoot, id, version, ct);
            if (template is null) problems.Add(refusal);
        }
        return problems.Count == 0 ? new(true, Status = "Designs in use are installed.", null, null) : Fail(string.Join(" ", problems.Distinct()));
    });

    // ---- helpers ----------------------------------------------------------------------------
    private const string Missing = "That design no longer exists.";
    private static readonly string TooMany = $"A show holds at most {GraphicsDesignIds.MaxDesigns} designs. Delete one you no longer use.";
    private bool Full() => _library.Document.Designs.Count >= GraphicsDesignIds.MaxDesigns;
    private GraphicsDesign? Find(string designId) => _library.Document.Designs.FirstOrDefault(d => d.DesignId == designId);
    private static string? CleanName(string? name) => name?.Trim() is { Length: > 0 and <= GraphicsDesignIds.MaxNameLength } clean ? clean : null;
    private string NewTemplateId(string name) => GraphicsDesignIds.NewTemplateId(name,
        _library.Document.Designs.Select(d => d.TemplateId).Concat(_library.Document.Templates.Select(t => t.TemplateId)).Concat(Available().Templates.Select(t => t.TemplateId)), _hex6);
    private static JsonObject Stamp(JsonElement definition, string templateId, int version, string name)
    {
        var manifest = JsonNode.Parse(definition.GetRawText())!.AsObject();
        manifest["templateId"] = templateId; manifest["templateVersion"] = version; manifest["name"] = name;
        return manifest;
    }

    private async Task<GraphicsDesignResult> Create(JsonObject manifest, string packageRoot, string name, CancellationToken ct)
    {
        var templateId = manifest["templateId"]!.GetValue<string>();
        var (template, refusal) = await Install(JsonSerializer.SerializeToElement(manifest), packageRoot, templateId, 1, ct);
        if (template is null) return Fail(refusal);
        var family = template.Family.Length > 0 ? template.Family : manifest["family"]?.GetValue<string>() ?? "";
        var design = new GraphicsDesign(templateId, name, family, templateId, 1, (template.Definition ?? JsonSerializer.SerializeToElement(manifest)).Clone(), [], _utcNow());
        if (!await Store([.. _library.Document.Designs, design], $"Created {name}.")) {
            await Remove(templateId, 1, ct);
            return Fail(_library.Status);
        }
        return new(true, Status = _library.Status, design, template);
    }

    private Task<bool> Store(IReadOnlyList<GraphicsDesign> designs, string success) => _library.SaveDesignsAsync(designs, success);

    /// <summary>Installed template for id+version, installing <paramref name="definition"/> when missing.</summary>
    private async Task<(NativeGraphicsTemplate? Template, string Refusal)> Install(JsonElement definition, string packageRoot, string templateId, int version, CancellationToken ct)
    {
        var epoch = _epoch;
        if (Current(epoch).Templates.FirstOrDefault(t => t.TemplateId == templateId && t.TemplateVersion == version) is { } present) return (present, "");
        var (installed, refusal) = await GraphicsTemplateInstall.RunAsync(Send, () => Current(epoch), _delay, _utcNow, definition.GetRawText(), packageRoot, templateId, version, ct);
        if (refusal is not null) return (null, refusal.Reason);
        return installed is null ? (null, "The design did not finish installing. Check the graphics library and try again.") : (installed, "");
    }

    /// <summary>Remove every installed version of <paramref name="templateId"/> (except <paramref name="keep"/>)
    /// that no playlist row, saved cue or live instance references. A refusal keeps that version.</summary>
    private async Task RemoveUnreferenced(string templateId, int? keep, CancellationToken ct)
    {
        var referenced = Pins(_library.Document, rowsAndCuesOnly: false);
        foreach (var version in Current(_epoch).Templates.Where(t => t.TemplateId == templateId && t.TemplateVersion != keep).Select(t => t.TemplateVersion).ToArray())
            if (!referenced.Contains((templateId, version))) await Remove(templateId, version, ct);
    }

    /// <summary>Native remove; template-library-busy never blocks, so retry it briefly (3 × 100 ms).</summary>
    private async Task<NativeGraphicsOperationResult> Remove(string templateId, int version, CancellationToken ct)
    {
        for (var attempt = 0; ; attempt++) {
            var result = await Send("graphics.template.remove", new() { ["templateId"] = templateId, ["templateVersion"] = version }, ct);
            if (result.Accepted || result.Reason != "template-library-busy" || attempt == 3) return result;
            await _delay(TimeSpan.FromMilliseconds(100), ct);
        }
    }

    private HashSet<(string Id, int Version)> Pins(GraphicsShowDocument document, bool rowsAndCuesOnly)
    {
        var pins = new HashSet<(string, int)>();
        foreach (var row in document.Playlist) {
            if (row.TemplateId.Length > 0) pins.Add((row.TemplateId, row.TemplateVersion));
            if (row.Image is { } image) pins.Add((image.TemplateId, image.TemplateVersion));
        }
        foreach (var cue in document.Cues) {
            if (cue.Draft is { } draft) pins.Add((draft.TemplateId, draft.TemplateVersion));
            if (cue.Draft?.Image is { } image) pins.Add((image.TemplateId, image.TemplateVersion));
        }
        if (rowsAndCuesOnly) return pins;
        foreach (var instance in Current(_epoch).Instances) {
            foreach (var version in new[] { instance.TemplateVersion, instance.CueTemplateVersion, instance.ProgramTemplateVersion }.Where(v => v > 0))
                pins.Add((instance.TemplateId, version));
            foreach (var image in new[] { instance.Image, instance.CueImage, instance.ProgramImage })
                if (image is not null) pins.Add((image.TemplateId, image.TemplateVersion));
        }
        return pins;
    }

    private async Task<GraphicsDesignResult> Run(Func<Task<GraphicsDesignResult>> action)
    {
        await _gate.WaitAsync();
        try {
            await _library.LoadAsync();
            _epoch = Available().CoreEpoch;
            return await action();
        }
        catch (InvalidOperationException ex) { return Fail(ex.Message); }
        catch (OperationCanceledException) { return Fail("Design operation cancelled. Nothing was replayed."); }
        finally { _gate.Release(); }
    }
    private GraphicsDesignResult Fail(string message) => new(false, Status = message, null, null);
    private NativeGraphicsState Available() => _observe() is { } state && state.CoreEpoch.Length > 0 ? state : throw new InvalidOperationException("Native Graphics is unavailable.");
    private NativeGraphicsState Current(string epoch) => _observe() is { } state && state.CoreEpoch == epoch
        ? state : throw new InvalidOperationException("Native core changed. The design operation was cancelled; nothing was replayed.");
    private Task<NativeGraphicsOperationResult> Send(string action, Dictionary<string, object?> body, CancellationToken ct)
    {
        Current(_epoch);
        body["schemaVersion"] = 1; body["clientId"] = ClientId; body.TryAdd("requestId", "design-" + Guid.NewGuid().ToString("N")); body["coreEpoch"] = _epoch;
        return _invoke(action, JsonSerializer.Serialize(body, Json), ct);
    }
}
