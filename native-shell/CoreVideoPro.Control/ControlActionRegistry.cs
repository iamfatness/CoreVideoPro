namespace CoreVideoPro.Control;

/// <summary>The authoritative, transport-agnostic catalog of every remotely-invocable action.
/// This is the single source of truth the OSC/HTTP transports validate against and the
/// Bitfocus Companion module is generated from (see <see cref="ControlManifest"/>).
///
/// Action ids are hand-authored and STABLE (never derived from C# method names), so the
/// remote API contract does not break when the ViewModel is refactored. A <see
/// cref="IControlSurface"/> implementation maps each id onto the actual VM command/property.
/// </summary>
public static class ControlActionRegistry
{
    private static readonly IReadOnlyList<ControlAction> AllActions = BuildActions();

    private static readonly IReadOnlyDictionary<string, ControlAction> ById =
        AllActions.ToDictionary(action => action.Id, StringComparer.Ordinal);

    public static IReadOnlyList<ControlAction> Actions => AllActions;

    public static bool TryGet(string actionId, out ControlAction action) => ById.TryGetValue(actionId, out action!);

    public static bool Contains(string actionId) => ById.ContainsKey(actionId);

    /// <summary>Validates and coerces raw positional args (as delivered by a transport) against
    /// the action's param schema. Returns the coerced values, or an error string. Extra trailing
    /// args are ignored; missing REQUIRED args are an error; optional missing args are null.</summary>
    public static bool TryBind(
        string actionId,
        IReadOnlyList<object?> rawArgs,
        out IReadOnlyList<object?> bound,
        out string? error)
    {
        bound = System.Array.Empty<object?>();
        if (!TryGet(actionId, out var action))
        {
            error = $"Unknown action '{actionId}'.";
            return false;
        }

        var result = new object?[action.Params.Count];
        for (var i = 0; i < action.Params.Count; i++)
        {
            var param = action.Params[i];
            var raw = i < rawArgs.Count ? rawArgs[i] : null;
            if (raw is null)
            {
                if (param.Required)
                {
                    error = $"Action '{actionId}' requires parameter '{param.Name}' at position {i}.";
                    return false;
                }

                result[i] = null;
                continue;
            }

            if (!TryCoerce(raw, param.Type, out var coerced))
            {
                error = $"Action '{actionId}' parameter '{param.Name}' must be {param.Type} (got '{raw}').";
                return false;
            }

            result[i] = coerced;
        }

        bound = result;
        error = null;
        return true;
    }

    private static bool TryCoerce(object raw, ControlParamType type, out object? value)
    {
        value = null;
        try
        {
            switch (type)
            {
                case ControlParamType.String:
                    value = raw as string ?? System.Convert.ToString(raw, System.Globalization.CultureInfo.InvariantCulture);
                    return value is not null;
                case ControlParamType.Int:
                    value = raw switch
                    {
                        int i => i,
                        long l => (int)l,
                        double d => (int)System.Math.Round(d),
                        float f => (int)System.MathF.Round(f),
                        bool b => b ? 1 : 0,
                        string s when int.TryParse(s, System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out var parsed) => parsed,
                        _ => (object?)null
                    };
                    return value is not null;
                case ControlParamType.Double:
                    value = raw switch
                    {
                        double d => d,
                        float f => (double)f,
                        int i => (double)i,
                        long l => (double)l,
                        string s when double.TryParse(s, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsed) => parsed,
                        _ => (object?)null
                    };
                    return value is not null;
                case ControlParamType.Bool:
                    value = raw switch
                    {
                        bool b => b,
                        int i => i != 0,
                        long l => l != 0,
                        double d => System.Math.Abs(d) > double.Epsilon,
                        float f => System.MathF.Abs(f) > float.Epsilon,
                        string s when bool.TryParse(s, out var parsed) => parsed,
                        string s when s is "1" => true,
                        string s when s is "0" => false,
                        _ => (object?)null
                    };
                    return value is not null;
                default:
                    return false;
            }
        }
        catch
        {
            return false;
        }
    }

    private static readonly ControlParam[] None = System.Array.Empty<ControlParam>();

    private static IReadOnlyList<ControlAction> BuildActions()
    {
        var s = ControlParamType.String;
        var i = ControlParamType.Int;
        var d = ControlParamType.Double;
        var b = ControlParamType.Bool;

        return new List<ControlAction>
        {
            // ---- Zoom session -----------------------------------------------------------
            new("zoom.join", "Join Zoom", "Join a Zoom meeting through the embedded Meeting SDK.",
                new[]
                {
                    new ControlParam("meetingUrl", s, true, "Zoom meeting URL or ID"),
                    new ControlParam("displayName", s, false, "Optional participant display name"),
                    new ControlParam("webinar", b, false, "Join as a webinar attendee")
                }),
            new("zoom.leave", "Leave Zoom", "Leave the current embedded Zoom meeting."),

            // ---- Transport --------------------------------------------------------------
            new("transport.take", "Take", "Cut/transition Preview to Program."),
            new("transport.transition.set", "Set transition", "Set the Take transition (cut/fade/dip/wipe).",
                new[] { new ControlParam("mode", s, true, "cut | fade | dip | wipe") }),
            new("transport.record.toggle", "Record toggle", "Toggle recording."),
            new("transport.record.set", "Record set", "Start or stop recording explicitly.",
                new[] { new ControlParam("on", b, true) }),
            new("transport.stream.toggle", "Stream toggle", "Toggle streaming."),
            new("transport.stream.set", "Stream set", "Start or stop streaming explicitly.",
                new[] { new ControlParam("on", b, true) }),
            new("transport.engine.toggle", "Engine toggle", "Toggle the capture engine on/off."),
            new("transport.engine.set", "Engine set", "Turn the capture engine on or off explicitly.",
                new[] { new ControlParam("on", b, true) }),
            new("transport.virtualcam.toggle", "Virtual camera toggle", "Toggle the virtual camera output."),
            new("transport.virtualcam.set", "Virtual camera set", "Turn the virtual camera on or off explicitly.",
                new[] { new ControlParam("on", b, true) }),
            new("transport.virtualcam.mirror.set", "Virtual camera mirror", "Mirror the virtual camera output horizontally.",
                new[] { new ControlParam("on", b, true) }),
            new("transport.virtualcam.name.set", "Virtual camera name", "Set the virtual camera's display name (blank keeps the default).",
                new[] { new ControlParam("name", s, false) }),

            // ---- Scenes / view ----------------------------------------------------------
            new("scene.select", "Select scene", "Cue a scene to Preview by id.",
                new[] { new ControlParam("sceneId", s, true) }),
            new("scene.dynamicGallery.create", "Create CoreVideo Tiles",
                "Create a dynamic CoreVideo Tiles scene and cue it to Preview."),
            new("view.setMode", "Set view mode", "Set the operator view (program/preview/programPreview/multiview).",
                new[] { new ControlParam("mode", s, true) }),

            // ---- Show Inputs ------------------------------------------------------------
            new("input.assign", "Assign input", "Assign a source to a Show Input slot (1-10).",
                new[]
                {
                    new ControlParam("slot", i, true, "1-10"),
                    new ControlParam("sourceId", s, true, "zoom:<pid> | capture:<id> | media:<id>")
                }),
            new("input.name", "Name input source", "Set the display name override for a Show Input's source (blank resets).",
                new[]
                {
                    new ControlParam("slot", i, true, "1-10"),
                    new ControlParam("name", s, false)
                }),
            new("input.inShow.set", "Input in-show", "Add/remove a Show Input slot from the show.",
                new[]
                {
                    new ControlParam("slot", i, true, "1-10"),
                    new ControlParam("on", b, true)
                }),

            // ---- Graphics / overlays ----------------------------------------------------
            new("graphics.lowerThird.toggle", "Lower-third toggle", "Toggle the program lower-third."),
            new("graphics.lowerThird.set", "Lower-third set", "Show or hide the program lower-third explicitly.",
                new[] { new ControlParam("on", b, true) }),
            new("graphics.caption.set", "Set caption", "Set the caption text (and optional speaker).",
                new[]
                {
                    new ControlParam("text", s, true),
                    new ControlParam("speaker", s, false)
                }),
            new("graphics.graphic.toggle", "Toggle graphic", "Toggle a graphic/overlay by id.",
                new[] { new ControlParam("graphicId", s, true) }),

            // ---- Audio ------------------------------------------------------------------
            new("audio.zoomMode.set", "Zoom audio mode", "Choose independently routed ISO stems or Zoom's combined fallback mix.",
                new[] { new ControlParam("mode", s, true, "perGuestIso | programMix") }),
            new("audio.monitor.set", "Audio monitor", "Enable/disable the audio monitor.",
                new[] { new ControlParam("on", b, true) }),
            new("audio.monitor.volume", "Monitor volume", "Set the audio monitor volume (0.0-1.0).",
                new[] { new ControlParam("volume", d, true, "0.0-1.0") }),
            new("audio.masterLimiter.set", "Master limiter", "Enable/disable the master limiter.",
                new[] { new ControlParam("on", b, true) }),
            new("audio.mastering.set", "Mastering", "Enable/disable the master-bus mastering chain.",
                new[] { new ControlParam("on", b, true) }),
            new("audio.mastering.target", "Mastering target", "Set the mastering loudness target (0=-14 streaming, 1=-16 podcast, 2=-23 broadcast).",
                new[] { new ControlParam("index", i, true, "0-2") }),
            new("audio.vst.scan", "Scan VST3 plug-ins", "Scan and validate installed VST3 plug-ins in isolated host processes."),

            // ---- Multiview --------------------------------------------------------------
            new("multiview.layout.set", "Multiview layout", "Set the multiview layout mode.",
                new[] { new ControlParam("mode", s, true, "grid | pgmPvwTop | pgmPvwLarge | pgmPvwSide") }),
            new("multiview.tileCount.set", "Multiview tiles", "Set the multiview source tile count.",
                new[] { new ControlParam("count", i, true, "1-10") }),
            new("multiview.showLabels.set", "Multiview labels", "Toggle multiview labels.",
                new[] { new ControlParam("on", b, true) }),
            new("multiview.showTally.set", "Multiview tally", "Toggle multiview tally borders.",
                new[] { new ControlParam("on", b, true) }),
            new("multiview.showMeters.set", "Multiview meters", "Toggle multiview audio meters.",
                new[] { new ControlParam("on", b, true) }),
            new("multiview.showClock.set", "Multiview clock", "Toggle the multiview clock.",
                new[] { new ControlParam("on", b, true) }),

            // ---- Automation -------------------------------------------------------------
            new("automation.toggle", "Automation toggle", "Toggle production automation on/off."),
            new("automation.autoTake.set", "Auto-take", "Toggle automation auto-take.",
                new[] { new ControlParam("on", b, true) }),
            new("automation.autoAssignInputs.set", "Auto-assign inputs", "Toggle auto-assigning roster participants to Show Inputs.",
                new[] { new ControlParam("on", b, true) }),
            new("automation.lowerThirds.set", "Auto lower-thirds", "Toggle automation lower-thirds.",
                new[] { new ControlParam("on", b, true) }),
            new("automation.captions.set", "Auto captions", "Toggle automation captions.",
                new[] { new ControlParam("on", b, true) }),

            // ---- Browser sources (BR-1) ---------------------------------------------------
            new("browser.add", "Add browser source", "Add a render-only browser source (WebView2 host process).",
                new[]
                {
                    new ControlParam("url", s, true, "http(s)://, file:// or data: URL"),
                    new ControlParam("width", i, false, "preset width (1920/1280/960); default 1920"),
                    new ControlParam("height", i, false, "preset height (1080/720/540); default 1080")
                }),
            new("browser.remove", "Remove browser source", "Remove a browser source by id.",
                new[] { new ControlParam("browserId", s, true, "browser:<n>") }),
            new("browser.reload", "Reload browser source", "Reload a browser source's page by id.",
                new[] { new ControlParam("browserId", s, true, "browser:<n>") }),
        };
    }
}
