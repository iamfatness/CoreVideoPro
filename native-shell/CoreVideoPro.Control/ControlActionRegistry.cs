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

    public static bool TryGet(string actionId, out ControlAction action) => ControlCatalog.StaticOnly.TryGet(actionId, out action);

    public static bool Contains(string actionId) => ControlCatalog.StaticOnly.Contains(actionId);

    /// <summary>Validates and coerces raw positional args (as delivered by a transport) against
    /// the action's param schema. Returns the coerced values, or an error string. Extra trailing
    /// args are ignored; missing REQUIRED args are an error; optional missing args are null.</summary>
    public static bool TryBind(
        string actionId,
        IReadOnlyList<object?> rawArgs,
        out IReadOnlyList<object?> bound,
        out string? error) => ControlCatalog.StaticOnly.TryBind(actionId, rawArgs, out bound, out error);

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
            new("transport.stream.rtmp.configure", "Configure RTMP destination", "Save RTMP/RTMPS settings without starting streaming. Refused while streaming or transitioning; omitted key retains the saved key, empty clears it.",
                new[] { new ControlParam("enabled", b), new ControlParam("protocol", s), new ControlParam("serverUrl", s), new ControlParam("streamKey", s, false) }),
            new("transport.stream.srt.configure", "Configure SRT destination", "Save SRT settings without starting streaming. Refused while streaming or transitioning; omitted passphrase retains it, empty clears it.",
                new[] { new ControlParam("enabled", b), new ControlParam("mode", s), new ControlParam("host", s), new ControlParam("port", s),
                    new ControlParam("latencyMs", s), new ControlParam("streamId", s), new ControlParam("keyLength", s), new ControlParam("passphrase", s, false) }),
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
            new("scene.cueWithRoutes", "Cue scene with routes",
                "Cue a scene to Preview and point named routes at Show Input slots. JSON object: routeId → slot number or null (empty box). Unknown scene, route or slot fails and changes nothing. Reply includes previewSceneId when applied.",
                new[]
                {
                    new ControlParam("sceneId", s, true),
                    new ControlParam("routes", s, true, "JSON object {\"ohg-host\":1,\"ohg-box-1\":null}")
                }),
            new("scene.route.set", "Set scene route",
                "Cue a scene to Preview and point one named route at a Show Input slot. Unknown scene, route or slot fails and changes nothing. Reply includes previewSceneId when applied.",
                new[]
                {
                    new ControlParam("sceneId", s, true),
                    new ControlParam("routeId", s, true, "Route id on that scene (for example ohg-host)"),
                    new ControlParam("slot", i, true, "Show Input slot 1-10")
                }),
            new("scene.dynamicGallery.create", "Create CoreVideo Tiles",
                "Create a dynamic CoreVideo Tiles scene and cue it to Preview."),
            new("scene.tiles.autofill.set", "Tiles automatic fill", "Set automatic roster fill for the Tiles scene in Preview.",
                new[] { new ControlParam("enabled", b, true) }),
            new("scene.tiles.slot.assign", "Assign Tiles slot", "Assign a canonical source id to a Preview Tiles slot; blank source clears the assignment.",
                new[] { new ControlParam("slot", d, true, "Integer 1-64"), new ControlParam("sourceId", s, false) }),
            new("scene.tiles.exclusion.set", "Tiles never show", "Exclude or allow a canonical source id in the Preview Tiles scene.",
                new[] { new ControlParam("sourceId", s, true), new ControlParam("excluded", b, true) }),
            new("scene.tiles.background.set", "Tiles background", "Set Preview Tiles color and optional live source. Scene media background remains available beneath it.",
                new[] { new ControlParam("color", s, true), new ControlParam("sourceId", s, false) }),
            new("scene.tiles.override.set", "Pin and crop tile", "Pin a Preview tile rectangle (0-1), set left/right crop percentages and order (0-63). Other tiles fill the largest remaining rectangle.",
                new[] { new ControlParam("sourceId", s, true), new ControlParam("x", d, true), new ControlParam("y", d, true),
                    new ControlParam("width", d, true), new ControlParam("height", d, true), new ControlParam("cropLeft", d, true),
                    new ControlParam("cropRight", d, true), new ControlParam("order", d, true) }),
            new("scene.tiles.override.clear", "Clear tile override", "Return a Preview tile to automatic grid positioning and crop.",
                new[] { new ControlParam("sourceId", s, true) }),
            new("view.setMode", "Set view mode", "Set the operator view (program/preview/program-preview/multiview); programPreview is accepted as a legacy alias.",
                new[] { new ControlParam("mode", s, true, ViewModeContract.SupportedModes) }),

            // ---- Show Inputs ------------------------------------------------------------
            new("input.assign", "Assign input", "Assign a source to a Show Input slot (1-10).",
                new[]
                {
                    new ControlParam("slot", i, true, "1-10"),
                    new ControlParam("sourceId", s, true, "zoom:<pid> | capture:<id> | media:<id>")
                }),
            new("source.capture.online.set", "Connect or take capture offline", "Use the existing capture device controls. Taking a device offline unassigns its inputs; native video readiness remains separately observed.",
                new[] { new ControlParam("sourceId", s, true, "capture:<deviceId>"), new ControlParam("online", b, true) }),
            new("input.unassign", "Unassign show input", "Clear only this input through the same operator command; the source remains available for reassignment.",
                new[] { new ControlParam("slot", i, true) }),
            new("input.name", "Name input source", "Set the display name override for a Show Input's source (blank resets).",
                new[]
                {
                    new ControlParam("slot", i, true, "1-10"),
                    new ControlParam("name", s, false)
                }),
            new("source.dropout.set", "Source on dropout", "Set what a source shows when it stops delivering (hold last frame or black). Zoom-only this slice.",
                new[]
                {
                    new ControlParam("sourceId", s, true, "zoom:<pid>"),
                    new ControlParam("policy", s, true, "hold | black")
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
            new("graphics.automation.configure", "Configure graphics rules", "Save scoped native graphics rules without arming them.",new[] {new ControlParam("request",s,true,"Versioned JSON graphics request")}),
            new("graphics.automation.control", "Control graphics automation", "Select mode, arm/disarm, hold, resume or hide automatic graphics.",new[] {new ControlParam("request",s,true,"Versioned JSON graphics request")}),
            new("graphics.authority.set", "Select graphics authority", "Explicitly select the legacy or instance authority for a migrated family.",
                new[] {new ControlParam("request", s, true, "Versioned JSON graphics request")}),
            new("graphics.instance.create", "Create graphic instance", "Create a hidden native graphic; admission is separate from preparation.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),
            new("graphics.instance.update", "Update graphic instance", "Prepare a cue or explicitly update Program without replaying the reveal.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),
            new("graphics.instance.cue", "Cue graphic instance", "Prepare the accepted revision for Preview without changing Program.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),
            new("graphics.instance.trigger", "Trigger graphic instance", "Show after readiness, hide, or clear an accepted native revision.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),
            new("graphics.clock.control", "Control graphics clock", "Configure, start, pause, resume or reset the native clock without showing it.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics clock request") }),
            new("graphics.instance.remove", "Remove graphic instance", "Remove a hidden native graphic instance.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),
            new("graphics.program.clear", "Clear native graphic instances", "Cancel pending native instance reveals and hide their Program layers.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request; explicit channel all") }),
            new("graphics.template.install", "Import graphics template", "Queue bounded native validation. Read templateInstalls for completion; admission is not installation.",
                new[] { new ControlParam("request", s, true, "Versioned JSON request with a manifest string or a local packagePath archive") }),
            new("graphics.template.list", "Observe graphics templates", "Read the native versioned catalog and import results without changing graphics.",
                new[] { new ControlParam("request", s, true, "Versioned JSON graphics request") }),

            new("graphics.template.select", "Select private graphics template", "Browse an installed template without creating a cue or changing Program.",
                new[] { new ControlParam("templateId", s, true), new ControlParam("version", i, true) }),
            new("graphics.cue.create", "Create hidden graphics cue", "Create and select a hidden cue from an installed template and its current private draft. Does not Show.",
                new[] { new ControlParam("templateId", s, true), new ControlParam("version", i, true), new ControlParam("scope", s, true, "global | scene"), new ControlParam("sceneId", s, false) }),
            new("graphics.studio.assign", "Assign lower third to Studio", "Pin the current private lower-third draft for the next Studio Show.",
                new[] { new ControlParam("followProgramSource", b, true) }),
            new("graphics.image.import", "Import local graphics image", "Verify a PNG/JPEG and browse it privately as a full-canvas graphic. Does not create a cue or show it.",
                new[] { new ControlParam("path", s, true) }),

            new("graphics.logo.import", "Upload graphic logo", "Verify a PNG/JPEG and apply it to the selected private logo draft. Does not Cue, Show or Update Program.",
                new[] { new ControlParam("path", s, true) }),

            new("graphics.cue.select", "Select graphics cue", "Select an existing cue for private editing.",
                new[] { new ControlParam("instanceId", s, true) }),
            new("graphics.cue.action", "Apply graphics cue action", "Use the selected workspace cue: cue, show, hide, or update. Each is explicit.",
                new[] { new ControlParam("action", s, true, "cue | show | hide | update") }),
            new("graphics.cue.save", "Save graphics cue", "Save the selected private cue with pinned template and image references.",
                new[] { new ControlParam("name", s, true) }),
            new("graphics.saved.restore", "Restore saved graphics cue", "Restore an unbound saved cue hidden; Cue and Show remain explicit.",
                new[] { new ControlParam("savedId", s, true) }),

            // ---- Graphics playout (issue #1076). Ids are the contract shared by the Graphics tab
            // buttons, the F-key router and Companion; never rename. Play In = take the graphic to
            // Program, To Preview = cue it (rides the next Take), Play Out = hide. --------------
            new("graphics.playlist.list", "List graphics playlist", "Read the playlist rows and the guest entries bound to Show Inputs. Changes nothing; reply carries data."),
            new("graphics.playlist.row.set", "Save graphics playlist row", "Add or replace one playlist row from its JSON (id, kind, name, hotkey, playOutAfterMs, templateId, templateVersion, fields, style, followSources, showClock). Program and Preview are unchanged.",
                new[] { new ControlParam("rowJson", s, true, "JSON object of one playlist row") }),
            new("graphics.playlist.clock", "Playlist clock transport", "Start, pause, resume or reset the countdown of a clock playlist row's instance without showing or hiding it. Refused until the row is cued or shown.",
                new[] { new ControlParam("rowId", s, true), new ControlParam("action", s, true, "start | pause | resume | reset") }),
            new("graphics.playlist.row.remove", "Remove graphics playlist row", "Remove one playlist row by id. An instance it already put on air stays until Play Out.",
                new[] { new ControlParam("rowId", s, true) }),
            new("graphics.playlist.hotkey.set", "Set graphics hotkey", "Assign F1..F12 to a playlist row (blank clears). A key already on another row moves to this one.",
                new[] { new ControlParam("rowId", s, true), new ControlParam("key", s, false, "F1..F12 or blank") }),
            new("graphics.playlist.default", "Install default graphics playlist", "Install the five starter rows (guest, title, logo, LIVE bug, sponsor) when no playlist exists. Refused when one exists.",
                new[] { new ControlParam("starterId", s, false, "Show starter id; default is the first starter") }),
            new("graphics.playIn", "Play In graphic", "Take a playlist row to Program: create or refresh its native instance, cue it, show from the cue. Guest rows take sourceId (zoom:/capture:/media:) or default to the current Program subject.",
                new[] { new ControlParam("rowId", s, true), new ControlParam("sourceId", s, false, "Guest rows: bound source id; blank = current Program subject") }),
            new("graphics.toPreview", "Graphic to Preview", "Cue a playlist row to Preview; it rides the next Take. Guest rows take sourceId or default to the current Program subject.",
                new[] { new ControlParam("rowId", s, true), new ControlParam("sourceId", s, false, "Guest rows: bound source id; blank = current Program subject") }),
            new("graphics.playOut", "Play Out graphic", "Hide one graphic on Program: by rowId (+sourceId for a guest entry) or by instanceId. Refused when it is not on Program.",
                new[] { new ControlParam("rowId", s, false), new ControlParam("sourceId", s, false), new ControlParam("instanceId", s, false, "Takes precedence over rowId") }),
            new("graphics.preview.remove", "Remove graphic from Preview", "Drop one cued graphic from Preview without touching Program.",
                new[] { new ControlParam("instanceId", s, true) }),
            new("graphics.playOutAll", "Play Out all graphics", "Hide every native graphic instance on Program (playlist, design and automatic alike)."),
            new("graphics.preview.clear", "Clear graphics Preview", "Drop every cued graphic from Preview; Program is unchanged."),
            new("graphics.strip", "Read graphics strip", "Read the on-air strip (onProgram, onPreview, auto, automationMode, automationArmed) from native instance truth. Changes nothing; reply carries data."),
            // Shot layouts (#1076 slice 2): one button = a scene on Preview plus its graphics, so one Take carries both.
            new("graphics.layout.list", "List shot layouts", "Read the shot layouts (id, name, sceneId, rowIds, hotkey, playOutOthers) and the scenes they may use. Changes nothing; reply carries data."),
            new("graphics.layout.set", "Save shot layout", "Add or replace one shot layout from its JSON (id, name, sceneId or blank, rowIds, hotkey Shift+F1..Shift+F11 or blank, playOutOthers). Refused when its scene or a row does not exist. A hotkey held by another layout moves here. Program and Preview are unchanged.",
                new[] { new ControlParam("layoutJson", s, true, "JSON object of one shot layout") }),
            new("graphics.layout.remove", "Remove shot layout", "Remove one shot layout by id. Program and Preview are unchanged.",
                new[] { new ControlParam("layoutId", s, true) }),
            new("graphics.layout.cue", "Cue shot layout", "Select the layout's scene to Preview and cue its playlist rows to Preview, so the next Take carries the picture and the graphics. PlayOutOthers layouts also play out Program graphics the layout does not carry. Refused whole when the scene or a row is missing.",
                new[] { new ControlParam("layoutId", s, true) }),
            new("graphics.layout.take", "Cue and Take shot layout", "graphics.layout.cue followed by the ordinary Take. A layout with no scene and no rows (Clean) only plays out.",
                new[] { new ControlParam("layoutId", s, true) }),

            // ---- Audio ------------------------------------------------------------------
            new("audio.zoomMode.set", "Zoom audio mode", "Choose independently routed ISO stems or Zoom's combined fallback mix.",
                new[] { new ControlParam("mode", s, true, "perGuestIso | programMix") }),
            new("audio.monitor.set", "Audio monitor", "Enable/disable the audio monitor.",
                new[] { new ControlParam("on", b, true),
                    new ControlParam("authorityEpoch", s, false),
                    new ControlParam("expectedRevision", d, false),
                    new ControlParam("operationId", s, false) }),
            new("settings.programBuffer.set", "Program buffer depth", "Save 2 or 3 frames for the next app launch. Does not change the running buffer. Read saved/session/restart state from /state.",
                [new("frames", ControlParamType.Double, Description: "Exactly 2 or 3; fractional values are rejected.")]),
            new("audio.monitor.volume", "Monitor volume", "Set the audio monitor volume (0.0-1.0).",
                new[] { new ControlParam("volume", d, true, "0.0-1.0"),
                    new ControlParam("authorityEpoch", s, false),
                    new ControlParam("expectedRevision", d, false),
                    new ControlParam("operationId", s, false) }),
            new("audio.route.set", "Audio crosspoint", "Set one source-to-bus route at an expected core revision. Read audioRouteAuthorityEpoch and audioRouteRevision from /state.",
                new[] { new ControlParam("sourceId", s, true),
                    new ControlParam("busId", s, true),
                    new ControlParam("enabled", b, true),
                    new ControlParam("gainDb", d, true, "-60 to +10 dB"),
                    new ControlParam("authorityEpoch", s, true),
                    new ControlParam("expectedRevision", d, true),
                    new ControlParam("operationId", s, true) }),
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
            new("automation.magic", "Magic Scene", "Run the one-shot Magic Scene command using the current source roster."),
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

            // ---- OHG show engine (shell-owned; the ohg.* actions come from the engine's own
            // manifest through ControlCatalog, never from this static list) ------------------
            new("showEngine.restart", "Restart show engine", "Restart the OHG show engine subprocess (used after it gives up, or after a config change)."),
        };
    }
}
