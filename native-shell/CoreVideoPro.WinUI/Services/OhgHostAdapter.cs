using System.Text.Json;
using CoreVideoPro.ShowEngine;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Applies the show engine's host commands to the shell (spec §8), through
/// <see cref="IOhgHostFacade"/>. Task 11 subscribes it to <c>ShowEngineBridge.HostCommand</c>
/// and marshals each call onto the <c>DispatcherQueue</c> — this class assumes it is already on
/// the UI thread and never marshals for itself.
///
/// **Three laws it enforces:**
/// 1. <see cref="ApplyAsync"/> NEVER throws. A malformed argument is a refusal string, not an
///    exception — the engine is a separate process and a protocol drift must not fail-fast a
///    live show (and a throwing <c>TryEnqueue</c> callback kills the process with no managed
///    log; see CLAUDE.md).
/// 2. It never blocks on a task. Takes are async, and <c>.GetAwaiter().GetResult()</c> on the
///    UI thread is a deadlock.
/// 3. In shadow mode (<c>shell.DriveHost == false</c>, the default) every command is recorded to
///    <see cref="ShadowLog"/> and NOTHING on the show is touched. The single exception is
///    <see cref="IOhgHostFacade.ReportStatus"/>, which is operator-visible text rather than show
///    state.
///
/// The <c>log</c> delegate receives every refusal and every status line, so Task 11 can route
/// them into the launch log.
///
/// **Wire shapes** come from <c>show-engine/src/host/stdioHostAdapter.ts</c> (the arg tuples) and
/// <c>protocol.ts</c> (whose <c>mapReplacer</c> serializes any <c>Map</c> as a [key, value] pair
/// array — which is why <c>applyLook</c>'s <c>boxes</c> and <c>setGallery</c>'s cells arrive as
/// arrays of two-element arrays). Field names are read from the engine's own types:
/// <c>LookPlacement</c> (<c>hostAdapter.ts</c>), <c>ProgramSource</c> (<c>contracts.ts</c>),
/// <c>Nameplate</c> (<c>lookDirector.ts</c>) and <c>QuestionOverlay</c>
/// (<c>overlayDirector.ts</c>).
/// </summary>
public sealed class OhgHostAdapter
{
    private const int ShadowLogCapacity = 50;

    internal const string GalleryNote = "gallery: cell order not applied (Tiles has no explicit order API)";

    private readonly IOhgHostFacade _facade;
    private readonly ShowShellConfig _shell;
    private readonly Action<string> _log;

    private readonly List<string> _shadowLog = new();

    /// <summary>lookId → the preset scene the look was last applied with. <c>setPreview</c>'s
    /// <c>{kind:"look"}</c> carries only the id, so this is the only place the shell can learn
    /// which scene to cue.</summary>
    private readonly Dictionary<string, string> _lookPresets = new(StringComparer.Ordinal);

    /// <summary>Reported (preset, sorted missing-route-set) keys — a scene that is simply missing
    /// a route would otherwise repeat its warning on every look change.</summary>
    private readonly HashSet<string> _reportedMissingRoutes = new(StringComparer.Ordinal);

    private bool _reportedGalleryNote;

    public OhgHostAdapter(IOhgHostFacade facade, ShowShellConfig shell, Action<string> log)
    {
        _facade = facade ?? throw new ArgumentNullException(nameof(facade));
        _shell = shell ?? throw new ArgumentNullException(nameof(shell));
        _log = log ?? throw new ArgumentNullException(nameof(log));
    }

    /// <summary>False = shadow mode: commands are recorded, never applied.</summary>
    public bool DriveHost => _shell.DriveHost;

    /// <summary>The newest <see cref="ShadowLogCapacity"/> recorded commands, oldest first.</summary>
    public IReadOnlyList<string> ShadowLog => _shadowLog;

    /// <summary>The most recently recorded command, or null.</summary>
    public string? ShadowLastCommand { get; private set; }

    /// <summary>Apply one host command. Never throws. Returns the refusal text, or null when the
    /// command was applied (possibly partially, with the shortfall reported).</summary>
    public async Task<string?> ApplyAsync(ShowEngineHostCommand command)
    {
        if (command is null)
        {
            return Refuse("unknown host command '<null>'");
        }

        try
        {
            // setGallery is recorded in BOTH modes (spec §8: "recorded to the shadow list
            // regardless of driveHost") — it is the one command 7a cannot apply at all.
            var isGallery = string.Equals(command.Name, "setGallery", StringComparison.Ordinal);
            if (!DriveHost || isGallery)
            {
                Record(command);
            }

            if (isGallery)
            {
                if (!_reportedGalleryNote)
                {
                    _reportedGalleryNote = true;
                    Status(GalleryNote);
                }

                return null;
            }

            if (!DriveHost)
            {
                // Shadow mode: recorded above, nothing else happens. Note that args are not even
                // parsed — shadow mode records what the engine SAID, verbatim.
                return IsKnownCommand(command.Name) ? null : Refuse($"unknown host command '{command.Name}'");
            }

            return command.Name switch
            {
                "assignSlot" => ApplyAssignSlot(command.Args),
                "applyLook" => ApplyLook(command.Args),
                "setPreview" => ApplySetPreview(command.Args),
                "cut" => await ApplyTakeAsync("cut", "cut").ConfigureAwait(true),
                "auto" => await ApplyAutoAsync(command.Args).ConfigureAwait(true),
                "setNameplates" => ApplySetNameplates(command.Args),
                "setQuestion" => ApplySetQuestion(command.Args),
                _ => Refuse($"unknown host command '{command.Name}'")
            };
        }
        catch (Exception ex)
        {
            // A protocol drift, a wrong arity, a wrong JSON type — never a crash, never silence.
            return Refuse($"{command.Name}: malformed args: {ex.Message}");
        }
    }

    private static bool IsKnownCommand(string name) => name switch
    {
        "assignSlot" or "applyLook" or "setPreview" or "cut" or "auto" or "setGallery"
            or "setNameplates" or "setQuestion" => true,
        _ => false
    };

    // ---- the §8 rows ----------------------------------------------------------------

    private string? ApplyAssignSlot(JsonElement args)
    {
        var slot = RequiredInt(args, 0, "slot");
        var participantId = OptionalString(Arg(args, 1), "participantId");

        return _facade.AssignZoomParticipant(slot, participantId)
            ? null
            : Refuse($"assignSlot: slot {slot} is outside 1..{_facade.ShowInputCount}");
    }

    private string? ApplyLook(JsonElement args)
    {
        var placement = RequiredObject(args, 0, "placement");
        var lookId = RequiredString(placement, "lookId");
        var scenePreset = RequiredString(placement, "scenePreset");

        if (!_facade.SceneExists(scenePreset))
        {
            // Refuse the WHOLE command: a look half-applied onto whatever scene happens to be
            // cued is worse than not applying it.
            return Refuse($"applyLook: preset scene '{scenePreset}' does not exist");
        }

        var routes = RoutesForLook(placement);
        _lookPresets[lookId] = scenePreset;

        var missing = _facade.CueSceneWithRoutes(scenePreset, routes);
        ReportMissingRoutes(scenePreset, missing, $"look '{lookId}': preset '{scenePreset}'");
        return null;
    }

    private string? ApplySetPreview(JsonElement args)
    {
        var source = RequiredObject(args, 0, "source");
        var kind = RequiredString(source, "kind");

        string sceneId;
        var routes = new Dictionary<string, int?>(StringComparer.Ordinal);

        switch (kind)
        {
            case "look":
            {
                var lookId = RequiredString(source, "lookId");
                if (!_lookPresets.TryGetValue(lookId, out var preset))
                {
                    return Refuse($"setPreview: look '{lookId}' has not been applied yet");
                }

                sceneId = preset;
                break;
            }

            case "slot":
            {
                var slot = RequiredInt(source, "slot");
                if (slot < 1)
                {
                    // The engine's parseProgramSource refuses this too; the adapter parses the
                    // source itself, so it must refuse it here as well rather than trusting.
                    return Refuse("setPreview: slot must be >= 1");
                }

                if (string.IsNullOrEmpty(_shell.Presets.Solo))
                {
                    return Refuse("setPreview: no solo preset configured");
                }

                sceneId = _shell.Presets.Solo!;
                routes["ohg-box-1"] = slot;
                break;
            }

            case "activeSpeaker":
            case "black":
            case "gallery":
            {
                var configured = kind switch
                {
                    "activeSpeaker" => _shell.Presets.ActiveSpeaker,
                    "black" => _shell.Presets.Black,
                    _ => _shell.Presets.Gallery
                };

                if (string.IsNullOrEmpty(configured))
                {
                    return Refuse($"setPreview: no {kind} preset configured");
                }

                sceneId = configured!;
                break;
            }

            default:
                return Refuse($"setPreview: unknown source kind '{kind}'");
        }

        if (!_facade.SceneExists(sceneId))
        {
            return Refuse($"setPreview: preset scene '{sceneId}' does not exist");
        }

        var missing = _facade.CueSceneWithRoutes(sceneId, routes);
        ReportMissingRoutes(sceneId, missing, $"setPreview: preset '{sceneId}'");
        return null;
    }

    private async Task<string?> ApplyTakeAsync(string commandName, string transition)
    {
        if (!_facade.CanTake)
        {
            return Refuse($"{commandName}: take unavailable");
        }

        var ok = await _facade.TakeAsync(transition).ConfigureAwait(true);
        return ok ? null : Refuse($"{commandName}: take refused by the shell");
    }

    private async Task<string?> ApplyAutoAsync(JsonElement args)
    {
        var requested = OptionalString(Arg(args, 0), "transition");
        var transition = requested ?? _shell.DefaultTransition;

        if (!_facade.IsKnownTransition(transition))
        {
            return Refuse($"auto: unknown transition '{transition}'");
        }

        return await ApplyTakeAsync("auto", transition).ConfigureAwait(true);
    }

    private string? ApplySetNameplates(JsonElement args)
    {
        var plates = RequiredArray(args, 0, "plates");
        var unassigned = new List<int>();

        foreach (var plate in plates.EnumerateArray())
        {
            var slot = RequiredInt(plate, "slot");
            var name = RequiredString(plate, "name");
            var location = RequiredString(plate, "location");

            var nameOk = _facade.SetInputDisplayName(slot, name);
            var titleOk = _facade.SetInputLowerThirdTitle(slot, location);
            if (!nameOk || !titleOk)
            {
                unassigned.Add(slot);
            }
        }

        if (unassigned.Count == 0)
        {
            return null;
        }

        var ids = string.Join(", ", unassigned);
        return Refuse(unassigned.Count == 1
            ? $"setNameplates: slot {ids} is not assigned"
            : $"setNameplates: slots {ids} are not assigned");
    }

    private string? ApplySetQuestion(JsonElement args)
    {
        var question = Arg(args, 0);
        var text = question.ValueKind == JsonValueKind.Null || question.ValueKind == JsonValueKind.Undefined
            ? string.Empty
            : RequiredString(question, "text");

        _facade.SetCaption(text);
        return null;
    }

    // ---- route naming (spec D11) ----------------------------------------------------

    /// <summary>
    /// The route ids a <c>LookPlacement</c> addresses: <c>"ohg-box-&lt;box&gt;"</c> for every box
    /// in the placement (value may be null — an empty box), plus <c>"ohg-host"</c> /
    /// <c>"ohg-reader"</c> ONLY when that chair is seated. A look with no reader must not touch
    /// the reader route: leaving the key out is how "not this look's business" is expressed, and
    /// it is why routes are addressed by ID rather than by layer position.
    /// </summary>
    public static IReadOnlyDictionary<string, int?> RoutesForLook(JsonElement placement)
    {
        var routes = new Dictionary<string, int?>(StringComparer.Ordinal);

        if (placement.ValueKind == JsonValueKind.Object
            && placement.TryGetProperty("boxes", out var boxes)
            && boxes.ValueKind == JsonValueKind.Array)
        {
            // A ReadonlyMap<number, number|null> on the wire (protocol.ts mapReplacer):
            // [[box, slot|null], …].
            foreach (var pair in boxes.EnumerateArray())
            {
                if (pair.ValueKind != JsonValueKind.Array || pair.GetArrayLength() < 2)
                {
                    throw new FormatException("boxes entries must be [box, slot|null] pairs");
                }

                var box = ReadInt(pair[0], "box");
                routes[$"ohg-box-{box}"] = ReadOptionalInt(pair[1], "slot");
            }
        }

        AddChair(routes, placement, "hostSlot", "ohg-host");
        AddChair(routes, placement, "readerSlot", "ohg-reader");
        return routes;
    }

    private static void AddChair(Dictionary<string, int?> routes, JsonElement placement, string field, string routeId)
    {
        if (placement.ValueKind != JsonValueKind.Object || !placement.TryGetProperty(field, out var value))
        {
            return;
        }

        var slot = ReadOptionalInt(value, field);
        if (slot.HasValue)
        {
            routes[routeId] = slot.Value;
        }
    }

    // ---- shadow log -----------------------------------------------------------------

    /// <summary>The shadow-log line for a command: <c>"&lt;seq&gt; &lt;name&gt;(&lt;args json&gt;)"</c>,
    /// with the args exactly as the engine serialized them.</summary>
    public static string FormatForShadow(ShowEngineHostCommand command)
        => $"{command.Seq} {command.Name}({command.Args.GetRawText()})";

    private void Record(ShowEngineHostCommand command)
    {
        var line = FormatForShadow(command);
        _shadowLog.Add(line);
        if (_shadowLog.Count > ShadowLogCapacity)
        {
            _shadowLog.RemoveRange(0, _shadowLog.Count - ShadowLogCapacity);
        }

        ShadowLastCommand = line;
    }

    // ---- reporting ------------------------------------------------------------------

    private void ReportMissingRoutes(string sceneId, IReadOnlyList<string>? missing, string prefix)
    {
        if (missing is null || missing.Count == 0)
        {
            return;
        }

        var sorted = missing.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        var ids = string.Join(", ", sorted);
        if (!_reportedMissingRoutes.Add($"{sceneId} {ids}"))
        {
            return;
        }

        Status($"{prefix} has no route(s) {ids}");
    }

    private string Refuse(string text)
    {
        _log(text);
        return text;
    }

    private void Status(string line)
    {
        _log(line);
        _facade.ReportStatus(line);
    }

    // ---- argument parsing (every failure becomes "<name>: malformed args: …") --------

    private static JsonElement Arg(JsonElement args, int index)
    {
        if (args.ValueKind != JsonValueKind.Array)
        {
            throw new FormatException($"expected an args array, got {args.ValueKind}");
        }

        return index < args.GetArrayLength() ? args[index] : default;
    }

    private static JsonElement RequiredArg(JsonElement args, int index, string name)
    {
        var value = Arg(args, index);
        if (value.ValueKind == JsonValueKind.Undefined)
        {
            throw new FormatException($"missing '{name}' at index {index}");
        }

        return value;
    }

    private static JsonElement RequiredObject(JsonElement args, int index, string name)
    {
        var value = RequiredArg(args, index, name);
        if (value.ValueKind != JsonValueKind.Object)
        {
            throw new FormatException($"'{name}' must be an object, got {value.ValueKind}");
        }

        return value;
    }

    private static JsonElement RequiredArray(JsonElement args, int index, string name)
    {
        var value = RequiredArg(args, index, name);
        if (value.ValueKind != JsonValueKind.Array)
        {
            throw new FormatException($"'{name}' must be an array, got {value.ValueKind}");
        }

        return value;
    }

    private static int RequiredInt(JsonElement args, int index, string name)
        => ReadInt(RequiredArg(args, index, name), name);

    private static int RequiredInt(JsonElement owner, string field)
    {
        if (owner.ValueKind != JsonValueKind.Object || !owner.TryGetProperty(field, out var value))
        {
            throw new FormatException($"missing '{field}'");
        }

        return ReadInt(value, field);
    }

    private static string RequiredString(JsonElement owner, string field)
    {
        if (owner.ValueKind != JsonValueKind.Object || !owner.TryGetProperty(field, out var value))
        {
            throw new FormatException($"missing '{field}'");
        }

        if (value.ValueKind != JsonValueKind.String)
        {
            throw new FormatException($"'{field}' must be a string, got {value.ValueKind}");
        }

        return value.GetString() ?? throw new FormatException($"'{field}' must be a string");
    }

    private static string? OptionalString(JsonElement value, string name) => value.ValueKind switch
    {
        JsonValueKind.String => value.GetString(),
        JsonValueKind.Null or JsonValueKind.Undefined => null,
        _ => throw new FormatException($"'{name}' must be a string or null, got {value.ValueKind}")
    };

    private static int ReadInt(JsonElement value, string name)
    {
        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out var parsed))
        {
            throw new FormatException($"'{name}' must be an integer, got {value.ValueKind}");
        }

        return parsed;
    }

    private static int? ReadOptionalInt(JsonElement value, string name) => value.ValueKind switch
    {
        JsonValueKind.Null or JsonValueKind.Undefined => null,
        _ => ReadInt(value, name)
    };
}
