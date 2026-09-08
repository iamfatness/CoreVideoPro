using System;
using System.Collections.Generic;
using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Pure projection of the show-engine's wire <c>ShowSnapshot</c> (a
/// <see cref="JsonElement"/>, straight off the wire per <c>show-engine/src/showSnapshot.ts</c> —
/// never a mirrored C# type) into the plain view records the shell renders from
/// (<c>Services/OhgSnapshotView.cs</c>).
///
/// <see cref="Project"/> is TOTAL: it never throws, regardless of what garbage arrives on the
/// wire. Every node read is guarded; a missing or wrong-kind node projects to its neutral value
/// (empty list, <c>"black"</c> for program/preview, <c>null</c>, health <c>"failing"</c>,
/// capability <c>"unavailable"</c>) and the guard appends a one-line warning naming the node it
/// could not read.</summary>
public static class OhgSnapshotProjection
{
    /// <summary>Total: never throws. Missing/malformed nodes project to empty/neutral values and
    /// are listed in <paramref name="warnings"/>.</summary>
    public static OhgSnapshotView Project(JsonElement snapshot, out IReadOnlyList<string> warnings)
    {
        var warningList = new List<string>();

        long revision = TryGetInt64(snapshot, "revision", warningList, "revision");

        // slotToParticipant: slot -> the participantId seated there (occupied slots only).
        // participantToSlot: participantId -> slot (the inverse, for resolving OhgPanelistRow.Slot
        // for entries drawn from panelists[]/unseated[] rather than slots[] itself).
        // slotToName: slot -> displayName, for gallery cells and look boxes to resolve without
        // re-walking slots[].
        var projectedSlots = ProjectSlots(snapshot, warningList, out var participantToSlot, out var slotToName);
        var onAirSlots = ProjectOnAirSlots(snapshot, warningList, out var onAirSet);
        var slots = ReapplyOnAir(projectedSlots, onAirSet);

        var panelists = ProjectPanelistList(snapshot, "panelists", warningList, participantToSlot);
        var unseated = ProjectPanelistList(snapshot, "unseated", warningList, participantToSlot);
        var gallery = ProjectGallery(snapshot, warningList, slotToName);
        var queue = ProjectQueue(snapshot, warningList);
        var program = ProjectProgram(snapshot, warningList);
        var look = ProjectLook(snapshot, warningList, slotToName);
        var manualBoxes = ProjectManualBoxes(snapshot, warningList);
        var overlays = ProjectOverlays(snapshot, warningList);
        var (registry, handsQueue, questionFeed) = ProjectCapabilities(snapshot, warningList);
        var health = ProjectHealth(snapshot, warningList);
        var smartGallery = TryGetBool(snapshot, "smartGallery", warningList, "smartGallery");
        var pagingRefused = TryGetNullableString(snapshot, "pagingRefused", warningList, "pagingRefused");
        var restoreWarnings = ProjectStringArray(snapshot, "restoreWarnings", warningList);

        warnings = warningList;

        return new OhgSnapshotView(
            revision, panelists, slots, gallery, queue, program, look, manualBoxes, onAirSlots,
            overlays, registry, handsQueue, questionFeed, health, smartGallery, unseated,
            pagingRefused, restoreWarnings);
    }

    /// <summary>The wire string for a ProgramSource object: {kind:"look",lookId} → "look:&lt;id&gt;",
    /// {kind:"slot",slot} → "slot:&lt;n&gt;", else kind.</summary>
    public static string FormatProgramSource(JsonElement source)
    {
        try
        {
            if (source.ValueKind != JsonValueKind.Object) return "black";
            if (!source.TryGetProperty("kind", out var kindEl) || kindEl.ValueKind != JsonValueKind.String)
                return "black";
            var kind = kindEl.GetString() ?? "black";

            switch (kind)
            {
                case "look":
                    if (source.TryGetProperty("lookId", out var lookIdEl) && lookIdEl.ValueKind == JsonValueKind.String)
                        return $"look:{lookIdEl.GetString()}";
                    return "black";
                case "slot":
                    if (source.TryGetProperty("slot", out var slotEl) && slotEl.ValueKind == JsonValueKind.Number && slotEl.TryGetInt32(out var slotNum))
                        return $"slot:{slotNum}";
                    return "black";
                case "black":
                case "gallery":
                case "activeSpeaker":
                    return kind;
                default:
                    return "black";
            }
        }
        catch
        {
            return "black";
        }
    }

    /// <summary>Pure: <c>engine.looks[]</c> → options (id, label ?? id); malformed entries
    /// skipped. Used by MainWindow (Task 10) to build the picker list.</summary>
    public static IReadOnlyList<OhgLookOption> LookOptionsFromEngine(JsonElement engine)
    {
        var options = new List<OhgLookOption>();
        try
        {
            if (engine.ValueKind != JsonValueKind.Object || !engine.TryGetProperty("looks", out var looksEl) ||
                looksEl.ValueKind != JsonValueKind.Array)
            {
                return options;
            }

            foreach (var entry in looksEl.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                if (!entry.TryGetProperty("id", out var idEl) || idEl.ValueKind != JsonValueKind.String) continue;
                var id = idEl.GetString();
                if (string.IsNullOrEmpty(id)) continue;

                string label = id;
                if (entry.TryGetProperty("label", out var labelEl) && labelEl.ValueKind == JsonValueKind.String)
                {
                    var labelValue = labelEl.GetString();
                    if (!string.IsNullOrEmpty(labelValue)) label = labelValue;
                }

                options.Add(new OhgLookOption(id, label));
            }
        }
        catch
        {
            // Total: any garbage yields whatever was parsed so far (empty on a top-level throw).
        }

        return options;
    }

    // ------------------------------------------------------------------------------------
    // Guarded scalar readers (never throw; return a caller-supplied fallback)
    // ------------------------------------------------------------------------------------

    private static long TryGetInt64(JsonElement obj, string name, List<string> warnings, string warnName)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) &&
                el.ValueKind == JsonValueKind.Number && el.TryGetInt64(out var value))
            {
                return value;
            }
        }
        catch { /* fall through to warning */ }

        warnings.Add($"missing or malformed node: {warnName}");
        return 0;
    }

    private static bool TryGetBool(JsonElement obj, string name, List<string> warnings, string warnName)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) &&
                (el.ValueKind == JsonValueKind.True || el.ValueKind == JsonValueKind.False))
            {
                return el.GetBoolean();
            }
        }
        catch { /* fall through to warning */ }

        warnings.Add($"missing or malformed node: {warnName}");
        return false;
    }

    private static string? TryGetNullableString(JsonElement obj, string name, List<string> warnings, string warnName)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el))
            {
                if (el.ValueKind == JsonValueKind.String) return el.GetString();
                if (el.ValueKind == JsonValueKind.Null) return null;
            }
        }
        catch { /* fall through to warning */ }

        warnings.Add($"missing or malformed node: {warnName}");
        return null;
    }

    private static string GetStringOr(JsonElement obj, string name, string fallback)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.String)
                return el.GetString() ?? fallback;
        }
        catch { }
        return fallback;
    }

    private static string? GetNullableString(JsonElement obj, string name)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el))
            {
                if (el.ValueKind == JsonValueKind.String) return el.GetString();
            }
        }
        catch { }
        return null;
    }

    private static bool GetBoolOr(JsonElement obj, string name, bool fallback)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) &&
                (el.ValueKind == JsonValueKind.True || el.ValueKind == JsonValueKind.False))
                return el.GetBoolean();
        }
        catch { }
        return fallback;
    }

    private static int? GetNullableInt(JsonElement obj, string name)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) &&
                el.ValueKind == JsonValueKind.Number && el.TryGetInt32(out var value))
                return value;
        }
        catch { }
        return null;
    }

    private static int GetIntOr(JsonElement obj, string name, int fallback)
    {
        try
        {
            if (obj.ValueKind == JsonValueKind.Object && obj.TryGetProperty(name, out var el) &&
                el.ValueKind == JsonValueKind.Number && el.TryGetInt32(out var value))
                return value;
        }
        catch { }
        return fallback;
    }

    // ------------------------------------------------------------------------------------
    // Panelists / slots / gallery
    // ------------------------------------------------------------------------------------

    private static OhgPanelistRow? ProjectPanelist(JsonElement panelist, IReadOnlyDictionary<string, int> participantToSlot)
    {
        if (panelist.ValueKind != JsonValueKind.Object) return null;

        var participantId = GetStringOr(panelist, "participantId", "");
        if (participantId.Length == 0) return null;

        int? seatedSlot = participantToSlot.TryGetValue(participantId, out var s) ? s : null;

        return new OhgPanelistRow(
            ParticipantId: participantId,
            DisplayName: GetStringOr(panelist, "displayName", ""),
            Location: GetStringOr(panelist, "location", ""),
            Pin: GetNullableString(panelist, "pin"),
            HasMukana: GetBoolOr(panelist, "hasMukana", false),
            Role: GetStringOr(panelist, "role", "panelist"),
            Online: GetBoolOr(panelist, "online", false),
            VideoOn: GetBoolOr(panelist, "videoOn", false),
            AudioOn: GetBoolOr(panelist, "audioOn", false),
            HandRaised: GetBoolOr(panelist, "handRaised", false),
            Slot: seatedSlot);
    }

    private static IReadOnlyList<OhgPanelistRow> ProjectPanelistList(
        JsonElement snapshot, string nodeName, List<string> warnings, IReadOnlyDictionary<string, int> participantToSlot)
    {
        var result = new List<OhgPanelistRow>();
        try
        {
            if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty(nodeName, out var arr) ||
                arr.ValueKind != JsonValueKind.Array)
            {
                warnings.Add($"missing or malformed node: {nodeName}");
                return result;
            }

            foreach (var entry in arr.EnumerateArray())
            {
                var row = ProjectPanelist(entry, participantToSlot);
                if (row != null) result.Add(row);
            }
        }
        catch
        {
            warnings.Add($"missing or malformed node: {nodeName}");
        }

        return result;
    }

    /// <summary>Projects <c>slots[]</c> and, along the way, builds the two lookups everything
    /// else needs: participantId -> slot (so panelists[]/unseated[] rows resolve their seat) and
    /// slot -> displayName (so gallery cells and look boxes resolve a seated name without walking
    /// slots[] again). OnAir is intentionally NOT set here — see <see cref="ReapplyOnAir"/>.</summary>
    private static List<OhgSlotRow> ProjectSlots(
        JsonElement snapshot, List<string> warnings,
        out Dictionary<string, int> participantToSlot, out Dictionary<int, string> slotToName)
    {
        var result = new List<OhgSlotRow>();
        participantToSlot = new Dictionary<string, int>();
        slotToName = new Dictionary<int, string>();

        try
        {
            if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty("slots", out var arr) ||
                arr.ValueKind != JsonValueKind.Array)
            {
                warnings.Add("missing or malformed node: slots");
                return result;
            }

            foreach (var entry in arr.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                var slotNum = GetIntOr(entry, "slot", 0);

                OhgPanelistRow? panelistRow = null;
                if (entry.TryGetProperty("panelist", out var p) && p.ValueKind == JsonValueKind.Object)
                {
                    var pid = GetStringOr(p, "participantId", "");
                    if (pid.Length > 0) participantToSlot[pid] = slotNum;

                    var name = GetStringOr(p, "displayName", "");
                    slotToName[slotNum] = name;

                    // participantToSlot is also used to resolve panelist rows' own Slot; the
                    // panelist embedded in this slot resolves to slotNum trivially, but sharing
                    // ProjectPanelist keeps the shape identical to panelists[]/unseated[] rows.
                    panelistRow = ProjectPanelist(p, participantToSlot);
                }

                result.Add(new OhgSlotRow(slotNum, panelistRow, OnAir: false));
            }
        }
        catch
        {
            warnings.Add("missing or malformed node: slots");
        }

        return result;
    }

    private static IReadOnlyList<OhgSlotRow> ReapplyOnAir(List<OhgSlotRow> slots, IReadOnlySet<int> onAirSlots)
    {
        var result = new List<OhgSlotRow>(slots.Count);
        foreach (var slot in slots)
        {
            result.Add(slot with { OnAir = onAirSlots.Contains(slot.Slot) });
        }
        return result;
    }

    private static IReadOnlyList<OhgGalleryCellRow> ProjectGallery(
        JsonElement snapshot, List<string> warnings, IReadOnlyDictionary<int, string> slotToName)
    {
        var result = new List<OhgGalleryCellRow>();
        try
        {
            if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty("gallery", out var arr) ||
                arr.ValueKind != JsonValueKind.Array)
            {
                warnings.Add("missing or malformed node: gallery");
                return result;
            }

            foreach (var entry in arr.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.Object) continue;
                var cell = GetIntOr(entry, "cell", 0);
                var slot = GetIntOr(entry, "slot", 0);
                string? displayName = (slot != 0 && slotToName.TryGetValue(slot, out var name)) ? name : null;
                result.Add(new OhgGalleryCellRow(cell, slot, displayName));
            }
        }
        catch
        {
            warnings.Add("missing or malformed node: gallery");
        }

        return result;
    }

    // ------------------------------------------------------------------------------------
    // Queue / Program / Look
    // ------------------------------------------------------------------------------------

    private static OhgQueueView ProjectQueue(JsonElement snapshot, List<string> warnings)
    {
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("queue", out var q) &&
                q.ValueKind == JsonValueKind.Object)
            {
                var previous = ProjectStringArray(q, "previous", warnings, silent: true);
                var current = GetNullableString(q, "current");
                var upcoming = ProjectStringArray(q, "upcoming", warnings, silent: true);
                return new OhgQueueView(previous, current, upcoming);
            }
        }
        catch { }

        warnings.Add("missing or malformed node: queue");
        return new OhgQueueView(Array.Empty<string>(), null, Array.Empty<string>());
    }

    private static OhgProgramView ProjectProgram(JsonElement snapshot, List<string> warnings)
    {
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("program", out var p) &&
                p.ValueKind == JsonValueKind.Object)
            {
                string program = "black";
                if (p.TryGetProperty("program", out var progEl))
                {
                    program = FormatProgramSourceWithWarning(progEl, warnings, "program.program");
                }
                else
                {
                    warnings.Add("missing or malformed node: program.program");
                }

                string preview = "black";
                if (p.TryGetProperty("preview", out var prevEl))
                {
                    preview = FormatProgramSourceWithWarning(prevEl, warnings, "program.preview");
                }
                else
                {
                    warnings.Add("missing or malformed node: program.preview");
                }

                bool follow = GetBoolOr(p, "activeSpeakerFollow", false);
                string? activeSpeakerId = GetNullableString(p, "activeSpeakerId");

                return new OhgProgramView(program, preview, follow, activeSpeakerId);
            }
        }
        catch { }

        warnings.Add("missing or malformed node: program");
        return new OhgProgramView("black", "black", false, null);
    }

    private static string FormatProgramSourceWithWarning(JsonElement source, List<string> warnings, string warnName)
    {
        var isExplicitBlack = source.ValueKind == JsonValueKind.Object &&
            source.TryGetProperty("kind", out var kindEl) && kindEl.ValueKind == JsonValueKind.String &&
            kindEl.GetString() == "black";

        var formatted = FormatProgramSource(source);
        if (formatted == "black" && !isExplicitBlack)
        {
            warnings.Add($"malformed or unknown program source: {warnName}");
        }
        return formatted;
    }

    private static OhgLookView? ProjectLook(JsonElement snapshot, List<string> warnings, IReadOnlyDictionary<int, string> slotToName)
    {
        try
        {
            if (snapshot.ValueKind != JsonValueKind.Object || !snapshot.TryGetProperty("look", out var look))
            {
                return null; // absent look is a legitimate neutral state (e.g. program not on a look), no warning
            }

            if (look.ValueKind == JsonValueKind.Null) return null;

            if (look.ValueKind != JsonValueKind.Object)
            {
                warnings.Add("missing or malformed node: look");
                return null;
            }

            var lookId = GetStringOr(look, "lookId", "");
            var scenePreset = GetStringOr(look, "scenePreset", "");
            var hostSlot = GetNullableInt(look, "hostSlot");
            var readerSlot = GetNullableInt(look, "readerSlot");
            var page = GetIntOr(look, "page", 0);
            var pageCount = GetIntOr(look, "pageCount", 0);
            var boxFill = GetStringOr(look, "boxFill", "queue");

            var boxes = new List<OhgBoxView>();
            if (look.TryGetProperty("boxes", out var boxesEl) && boxesEl.ValueKind == JsonValueKind.Array)
            {
                foreach (var boxEl in boxesEl.EnumerateArray())
                {
                    if (boxEl.ValueKind != JsonValueKind.Object) continue;
                    var box = GetIntOr(boxEl, "box", 0);
                    var slot = GetNullableInt(boxEl, "slot");
                    string? displayName = (slot is int s && s != 0 && slotToName.TryGetValue(s, out var name)) ? name : null;
                    boxes.Add(new OhgBoxView(box, slot, displayName));
                }
            }

            return new OhgLookView(lookId, scenePreset, hostSlot, readerSlot, boxes, page, pageCount, boxFill);
        }
        catch
        {
            warnings.Add("missing or malformed node: look");
            return null;
        }
    }

    // ------------------------------------------------------------------------------------
    // ManualBoxes / OnAirSlots / Overlays / Capabilities / Health
    // ------------------------------------------------------------------------------------

    private static IReadOnlyDictionary<int, int> ProjectManualBoxes(JsonElement snapshot, List<string> warnings)
    {
        var result = new Dictionary<int, int>();
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("manualBoxes", out var mb) &&
                mb.ValueKind == JsonValueKind.Object)
            {
                foreach (var prop in mb.EnumerateObject())
                {
                    if (int.TryParse(prop.Name, out var box) && prop.Value.ValueKind == JsonValueKind.Number &&
                        prop.Value.TryGetInt32(out var slot))
                    {
                        result[box] = slot;
                    }
                }
                return result;
            }
        }
        catch { }

        warnings.Add("missing or malformed node: manualBoxes");
        return result;
    }

    private static IReadOnlyList<int> ProjectOnAirSlots(JsonElement snapshot, List<string> warnings, out HashSet<int> onAirSet)
    {
        onAirSet = new HashSet<int>();
        var result = new List<int>();
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("tally", out var tally) &&
                tally.ValueKind == JsonValueKind.Object && tally.TryGetProperty("onAirSlots", out var arr) &&
                arr.ValueKind == JsonValueKind.Array)
            {
                foreach (var el in arr.EnumerateArray())
                {
                    if (el.ValueKind == JsonValueKind.Number && el.TryGetInt32(out var slot))
                    {
                        result.Add(slot);
                        onAirSet.Add(slot);
                    }
                }
                return result;
            }
        }
        catch { }

        warnings.Add("missing or malformed node: tally.onAirSlots");
        return result;
    }

    private static OhgOverlayView ProjectOverlays(JsonElement snapshot, List<string> warnings)
    {
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("overlays", out var o) &&
                o.ValueKind == JsonValueKind.Object)
            {
                string? questionText = null;
                string? questionAsker = null;
                if (o.TryGetProperty("question", out var q) && q.ValueKind == JsonValueKind.Object)
                {
                    questionText = GetNullableString(q, "text");
                    questionAsker = GetNullableString(q, "askerName");
                }

                string? headlineName = null;
                string? headlineLocation = null;
                if (o.TryGetProperty("headline", out var h) && h.ValueKind == JsonValueKind.Object)
                {
                    headlineName = GetNullableString(h, "name");
                    headlineLocation = GetNullableString(h, "location");
                }

                bool headlineVisible = GetBoolOr(o, "headlineVisible", false);

                return new OhgOverlayView(questionText, questionAsker, headlineName, headlineLocation, headlineVisible);
            }
        }
        catch { }

        warnings.Add("missing or malformed node: overlays");
        return new OhgOverlayView(null, null, null, null, false);
    }

    private static OhgCapabilityView ProjectCapability(JsonElement caps, string name, List<string> warnings)
    {
        try
        {
            if (caps.ValueKind == JsonValueKind.Object && caps.TryGetProperty(name, out var c) &&
                c.ValueKind == JsonValueKind.Object)
            {
                var state = GetStringOr(c, "state", "unavailable");
                var detail = GetNullableString(c, "detail");
                return new OhgCapabilityView(state, detail);
            }
        }
        catch { }

        warnings.Add($"missing or malformed node: capabilities.{name}");
        return new OhgCapabilityView("unavailable", null);
    }

    private static (OhgCapabilityView registry, OhgCapabilityView handsQueue, OhgCapabilityView questionFeed)
        ProjectCapabilities(JsonElement snapshot, List<string> warnings)
    {
        JsonElement caps = default;
        bool hasCaps;
        try
        {
            hasCaps = snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("capabilities", out caps) &&
                      caps.ValueKind == JsonValueKind.Object;
        }
        catch
        {
            hasCaps = false;
        }

        if (!hasCaps)
        {
            warnings.Add("missing or malformed node: capabilities");
            var neutral = new OhgCapabilityView("unavailable", null);
            return (neutral, neutral, neutral);
        }

        return (
            ProjectCapability(caps, "registry", warnings),
            ProjectCapability(caps, "handsQueue", warnings),
            ProjectCapability(caps, "questionFeed", warnings));
    }

    private static string ProjectHealthState(JsonElement health, string endpoint, List<string> warnings)
    {
        try
        {
            if (health.ValueKind == JsonValueKind.Object && health.TryGetProperty(endpoint, out var e) &&
                e.ValueKind == JsonValueKind.Object)
            {
                return GetStringOr(e, "state", "failing");
            }
        }
        catch { }

        warnings.Add($"missing or malformed node: health.{endpoint}");
        return "failing";
    }

    private static readonly Dictionary<string, int> HealthRank = new() { ["ok"] = 0, ["dormant"] = 1, ["failing"] = 2 };

    private static int RankOf(string state) => HealthRank.TryGetValue(state, out var rank) ? rank : 2;

    private static OhgHealthView ProjectHealth(JsonElement snapshot, List<string> warnings)
    {
        JsonElement health = default;
        bool hasHealth;
        try
        {
            hasHealth = snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty("health", out health) &&
                        health.ValueKind == JsonValueKind.Object;
        }
        catch
        {
            hasHealth = false;
        }

        if (!hasHealth)
        {
            warnings.Add("missing or malformed node: health");
            return new OhgHealthView("failing", "failing", "failing", "failing");
        }

        var panelists = ProjectHealthState(health, "panelists", warnings);
        var hands = ProjectHealthState(health, "hands", warnings);
        var question = ProjectHealthState(health, "question", warnings);

        // Mirrors worstMukanaHealth in show-engine/src/controlState.ts: failing > dormant > ok.
        var worst = "ok";
        foreach (var state in new[] { panelists, hands, question })
        {
            if (RankOf(state) > RankOf(worst)) worst = state;
        }

        return new OhgHealthView(panelists, hands, question, worst);
    }

    private static IReadOnlyList<string> ProjectStringArray(JsonElement snapshot, string nodeName, List<string> warnings, bool silent = false)
    {
        var result = new List<string>();
        try
        {
            if (snapshot.ValueKind == JsonValueKind.Object && snapshot.TryGetProperty(nodeName, out var arr) &&
                arr.ValueKind == JsonValueKind.Array)
            {
                foreach (var el in arr.EnumerateArray())
                {
                    if (el.ValueKind == JsonValueKind.String)
                    {
                        var s = el.GetString();
                        if (s != null) result.Add(s);
                    }
                }
                return result;
            }
        }
        catch { }

        if (!silent) warnings.Add($"missing or malformed node: {nodeName}");
        return result;
    }
}
