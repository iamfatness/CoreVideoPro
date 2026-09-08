using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.ShowEngine;

namespace CoreVideoPro.WinUI.Services;

/// <summary>One <c>engine.looks[]</c> entry, editable form. Mirrors <c>LookDefinition</c>
/// (<c>show-engine/src/contracts.ts</c>) field for field; defaults match <c>config.ts</c>'s
/// optional-field defaults (<c>plateTone</c> "neutral", <c>tallySource</c> "boxes", <c>boxFill</c>
/// "queue").</summary>
public sealed class OhgLookEdit
{
    public string Id = "";
    public string Label = "";
    public string? ScenePreset;
    public int Boxes;
    public bool IncludesHost;
    public bool IncludesReader;
    public string PlateTone = "neutral";
    public string TallySource = "boxes";
    public string BoxFill = "queue";
}

/// <summary>The three look enumerations, copied from <c>show-engine/src/contracts.ts</c>
/// (<c>PLATE_TONES</c> / <c>TALLY_SOURCES</c> / <c>BOX_FILLS</c>).
///
/// <para><b>Why this is one place.</b> <c>optionalPlateTone</c> and its siblings in
/// <c>show-engine/src/config.ts</c> THROW on any value outside these sets, and a throwing config
/// parse is exit 78 — terminal, no respawn. A settings picker offering a value the engine does not
/// know (this shipped once, as <c>"warm"</c>/<c>"cool"</c> plate tones) therefore lets the operator
/// save a config that kills the engine on its next launch. The picker choices and the Save-time
/// validation both read these arrays, and <c>OhgSettingsChoicesTests</c> pins them against literal
/// copies of the engine's arrays so drift fails a test instead of a show.</para></summary>
internal static class OhgLookChoices
{
    internal static readonly string[] PlateTones = ["neutral", "accent", "guest", "breaking"];
    internal static readonly string[] TallySources = ["boxes", "activeSpeaker"];
    internal static readonly string[] BoxFills = ["queue", "manual"];

    internal static string List(IReadOnlyList<string> choices) => string.Join(", ", choices);
}

/// <summary>Editable form of a <see cref="ShowConfig"/> (Plan 7b Task 9). Plain mutable class —
/// no <see cref="System.ComponentModel.INotifyPropertyChanged"/> here; <c>OhgSettingsViewModel</c>
/// wraps the parts that need binding.
///
/// <para><b>Unknown fields survive editing.</b> <see cref="FromConfig"/> reads every field
/// <c>show-engine/src/config.ts</c> knows about; anything else on <c>engine</c> — including
/// <c>statePath</c>, which the edit model deliberately does not own (the store's
/// <c>MaterializeEngineConfig</c> fills it at launch) — is cloned verbatim into <see cref="Extra"/>
/// and re-emitted by <see cref="ToConfig"/>. A config edited through this model never silently
/// drops a field the engine understands but the UI doesn't expose yet.</para></summary>
public sealed class OhgConfigEditModel
{
    public bool RegistryEnabled;
    public bool HandsQueueEnabled;
    public bool QuestionFeedEnabled;

    public string? MukanaBaseUrl;
    public string? MukanaEvent;
    public int PanelistsIntervalMs = 5000;
    public int HandsIntervalMs = 2000;
    public int QuestionIntervalMs = 2000;
    public int MaxBackoffMs = 60000;

    /// <summary>Read-only in the UI (the host capacity is fixed at 10); still read from and
    /// written back to the config so a round trip never mutates it.</summary>
    public int Capacity = 10;

    public int UtilityPinBase = 9000;
    public int GalleryCells = 16;
    public List<string> SkipRoles = ["aslinterpreter"];
    public List<OhgLookEdit> Looks = [];

    public bool DriveHost;
    public string? PresetSolo;
    public string? PresetActiveSpeaker;
    public string? PresetBlack;
    public string? PresetGallery;
    public string DefaultTransition = "cut";
    public string? TallyUrl;

    /// <summary>Every <c>engine</c> property this model does not know about (plus
    /// <c>statePath</c>), cloned so it survives independently of the source document.</summary>
    public Dictionary<string, JsonElement> Extra = new(StringComparer.Ordinal);

    /// <summary>Reads every known <c>engine</c> field from <paramref name="config"/>, and the
    /// shell fields from <c>config.Shell</c>. A present-but-malformed known field is skipped
    /// (the model keeps its default) and reported in <paramref name="warnings"/>; a MISSING known
    /// field is not a warning — that's an ordinary default, same as the engine parser.</summary>
    public static OhgConfigEditModel FromConfig(ShowConfig config, out IReadOnlyList<string> warnings)
    {
        var problems = new List<string>();
        var model = new OhgConfigEditModel { Looks = [], Extra = new Dictionary<string, JsonElement>(StringComparer.Ordinal) };

        var engine = config.Engine;
        if (engine.ValueKind == JsonValueKind.Object)
        {
            foreach (var property in engine.EnumerateObject())
            {
                switch (property.Name)
                {
                    case "capacity":
                        if (TryReadInt(property.Value, out var capacity))
                        {
                            model.Capacity = capacity;
                        }
                        else
                        {
                            problems.Add("engine.capacity: expected an integer");
                        }
                        break;

                    case "utilityPinBase":
                        if (TryReadInt(property.Value, out var pinBase))
                        {
                            model.UtilityPinBase = pinBase;
                        }
                        else
                        {
                            problems.Add("engine.utilityPinBase: expected an integer");
                        }
                        break;

                    case "galleryCells":
                        if (TryReadInt(property.Value, out var galleryCells))
                        {
                            model.GalleryCells = galleryCells;
                        }
                        else
                        {
                            problems.Add("engine.galleryCells: expected an integer");
                        }
                        break;

                    case "skipRoles":
                        if (TryReadStringArray(property.Value, out var roles))
                        {
                            model.SkipRoles = roles;
                        }
                        else
                        {
                            problems.Add("engine.skipRoles: expected an array of strings");
                        }
                        break;

                    case "integrations":
                        if (property.Value.ValueKind == JsonValueKind.Object)
                        {
                            model.RegistryEnabled = ReadOptionalBool(property.Value, "registry", false, problems, "engine.integrations.registry");
                            model.HandsQueueEnabled = ReadOptionalBool(property.Value, "handsQueue", false, problems, "engine.integrations.handsQueue");
                            model.QuestionFeedEnabled = ReadOptionalBool(property.Value, "questionFeed", false, problems, "engine.integrations.questionFeed");
                        }
                        else
                        {
                            problems.Add("engine.integrations: expected an object");
                        }
                        break;

                    case "mukana":
                        if (property.Value.ValueKind == JsonValueKind.Object)
                        {
                            ReadMukana(property.Value, model, problems);
                        }
                        else if (property.Value.ValueKind != JsonValueKind.Null)
                        {
                            problems.Add("engine.mukana: expected an object or null");
                        }
                        break;

                    case "looks":
                        if (property.Value.ValueKind == JsonValueKind.Array)
                        {
                            var looks = new List<OhgLookEdit>();
                            foreach (var lookElement in property.Value.EnumerateArray())
                            {
                                looks.Add(ParseLook(lookElement, problems));
                            }
                            model.Looks = looks;
                        }
                        else
                        {
                            problems.Add("engine.looks: expected an array");
                        }
                        break;

                    default:
                        // Includes "statePath" (owned by ShowConfigStore.MaterializeEngineConfig,
                        // never by this model) and any field this model does not know about yet.
                        model.Extra[property.Name] = property.Value.Clone();
                        break;
                }
            }
        }
        else
        {
            problems.Add("engine: expected an object");
        }

        model.DriveHost = config.Shell.DriveHost;
        model.PresetSolo = config.Shell.Presets.Solo;
        model.PresetActiveSpeaker = config.Shell.Presets.ActiveSpeaker;
        model.PresetBlack = config.Shell.Presets.Black;
        model.PresetGallery = config.Shell.Presets.Gallery;
        model.DefaultTransition = config.Shell.DefaultTransition;
        model.TallyUrl = config.Shell.TallyUrl;

        warnings = problems;
        return model;
    }

    /// <summary>Builds the engine <see cref="JsonElement"/> (<c>mukana:null</c> when every
    /// integration flag is off, per <c>config.ts</c>'s omissible-mukana rule) and the shell block
    /// from this model's fields, plus every <see cref="Extra"/> entry re-emitted verbatim.</summary>
    public ShowConfig ToConfig()
    {
        var engine = new JsonObject
        {
            ["capacity"] = Capacity,
            ["utilityPinBase"] = UtilityPinBase,
            ["galleryCells"] = GalleryCells,
            ["skipRoles"] = new JsonArray(SkipRoles.Select(role => (JsonNode?)JsonValue.Create(role)).ToArray()),
            ["integrations"] = new JsonObject
            {
                ["registry"] = RegistryEnabled,
                ["handsQueue"] = HandsQueueEnabled,
                ["questionFeed"] = QuestionFeedEnabled
            },
            ["looks"] = new JsonArray(Looks.Select(look => (JsonNode?)new JsonObject
            {
                ["id"] = look.Id,
                ["label"] = look.Label,
                ["scenePreset"] = look.ScenePreset ?? "",
                ["boxes"] = look.Boxes,
                ["includesHost"] = look.IncludesHost,
                ["includesReader"] = look.IncludesReader,
                ["plateTone"] = look.PlateTone,
                ["tallySource"] = look.TallySource,
                ["boxFill"] = look.BoxFill
            }).ToArray())
        };

        if (RegistryEnabled || HandsQueueEnabled || QuestionFeedEnabled)
        {
            engine["mukana"] = new JsonObject
            {
                ["baseUrl"] = MukanaBaseUrl ?? "",
                ["event"] = MukanaEvent ?? "",
                ["panelistsIntervalMs"] = PanelistsIntervalMs,
                ["handsIntervalMs"] = HandsIntervalMs,
                ["questionIntervalMs"] = QuestionIntervalMs,
                ["maxBackoffMs"] = MaxBackoffMs
            };
        }
        else
        {
            engine["mukana"] = null;
        }

        foreach (var (key, value) in Extra)
        {
            engine[key] = JsonNode.Parse(value.GetRawText());
        }

        var engineElement = JsonDocument.Parse(engine.ToJsonString()).RootElement.Clone();

        return new ShowConfig
        {
            Version = ShowConfig.CurrentVersion,
            Engine = engineElement,
            Shell = new ShowShellConfig
            {
                DriveHost = DriveHost,
                Presets = new ShowPresetScenes
                {
                    Solo = PresetSolo,
                    ActiveSpeaker = PresetActiveSpeaker,
                    Black = PresetBlack,
                    Gallery = PresetGallery
                },
                DefaultTransition = DefaultTransition,
                TallyUrl = TallyUrl
            }
        };
    }

    /// <summary>The spec's four fixed looks, empty <c>scenePreset</c> (the operator must assign
    /// each to a scene before the config validates).</summary>
    public static OhgConfigEditModel Default()
    {
        return new OhgConfigEditModel
        {
            Capacity = 10,
            UtilityPinBase = 9000,
            GalleryCells = 16,
            SkipRoles = ["aslinterpreter"],
            RegistryEnabled = false,
            HandsQueueEnabled = false,
            QuestionFeedEnabled = false,
            MukanaBaseUrl = null,
            MukanaEvent = null,
            DriveHost = false,
            Looks =
            [
                new OhgLookEdit { Id = "hr-q", Label = "HR + Q", Boxes = 1, IncludesHost = true, IncludesReader = false },
                new OhgLookEdit { Id = "banter", Label = "Banter", Boxes = 2 },
                new OhgLookEdit { Id = "teatime", Label = "Teatime", Boxes = 3, IncludesReader = true },
                new OhgLookEdit { Id = "panel-checks", Label = "Panel Checks", Boxes = 4, TallySource = "activeSpeaker" }
            ]
        };
    }

    private static bool TryReadInt(JsonElement element, out int value)
    {
        if (element.ValueKind == JsonValueKind.Number && element.TryGetInt32(out value))
        {
            return true;
        }
        value = 0;
        return false;
    }

    private static bool TryReadStringArray(JsonElement element, out List<string> values)
    {
        values = [];
        if (element.ValueKind != JsonValueKind.Array)
        {
            return false;
        }

        foreach (var entry in element.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.String)
            {
                return false;
            }
            values.Add(entry.GetString() ?? "");
        }
        return true;
    }

    private static bool ReadOptionalBool(JsonElement parent, string key, bool fallback, List<string> problems, string label)
    {
        if (!parent.TryGetProperty(key, out var value))
        {
            return fallback;
        }
        if (value.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            return value.GetBoolean();
        }
        problems.Add($"{label}: expected a boolean");
        return fallback;
    }

    private static void ReadMukana(JsonElement mukana, OhgConfigEditModel model, List<string> problems)
    {
        if (mukana.TryGetProperty("baseUrl", out var baseUrlElement) && baseUrlElement.ValueKind == JsonValueKind.String)
        {
            model.MukanaBaseUrl = baseUrlElement.GetString();
        }
        else
        {
            problems.Add("engine.mukana.baseUrl: expected a string");
        }

        if (mukana.TryGetProperty("event", out var eventElement) && eventElement.ValueKind == JsonValueKind.String)
        {
            model.MukanaEvent = eventElement.GetString();
        }
        else
        {
            problems.Add("engine.mukana.event: expected a string");
        }

        model.PanelistsIntervalMs = ReadOptionalInt(mukana, "panelistsIntervalMs", 5000, problems, "engine.mukana.panelistsIntervalMs");
        model.HandsIntervalMs = ReadOptionalInt(mukana, "handsIntervalMs", 2000, problems, "engine.mukana.handsIntervalMs");
        model.QuestionIntervalMs = ReadOptionalInt(mukana, "questionIntervalMs", 2000, problems, "engine.mukana.questionIntervalMs");
        model.MaxBackoffMs = ReadOptionalInt(mukana, "maxBackoffMs", 60000, problems, "engine.mukana.maxBackoffMs");
    }

    private static int ReadOptionalInt(JsonElement parent, string key, int fallback, List<string> problems, string label)
    {
        if (!parent.TryGetProperty(key, out var value))
        {
            return fallback;
        }
        if (TryReadInt(value, out var parsed))
        {
            return parsed;
        }
        problems.Add($"{label}: expected an integer");
        return fallback;
    }

    private static OhgLookEdit ParseLook(JsonElement lookElement, List<string> problems)
    {
        var look = new OhgLookEdit();

        if (lookElement.ValueKind != JsonValueKind.Object)
        {
            problems.Add("engine.looks[]: expected an object");
            return look;
        }

        if (lookElement.TryGetProperty("id", out var idElement) && idElement.ValueKind == JsonValueKind.String)
        {
            look.Id = idElement.GetString() ?? "";
        }
        else
        {
            problems.Add("engine.looks[].id: expected a string");
        }

        if (lookElement.TryGetProperty("label", out var labelElement) && labelElement.ValueKind == JsonValueKind.String)
        {
            look.Label = labelElement.GetString() ?? "";
        }
        else
        {
            problems.Add("engine.looks[].label: expected a string");
        }

        if (lookElement.TryGetProperty("scenePreset", out var presetElement) && presetElement.ValueKind == JsonValueKind.String)
        {
            look.ScenePreset = presetElement.GetString();
        }
        else
        {
            problems.Add("engine.looks[].scenePreset: expected a string");
        }

        if (lookElement.TryGetProperty("boxes", out var boxesElement))
        {
            if (TryReadInt(boxesElement, out var boxes))
            {
                look.Boxes = boxes;
            }
            else
            {
                problems.Add("engine.looks[].boxes: expected an integer");
            }
        }

        if (lookElement.TryGetProperty("includesHost", out var hostElement) && hostElement.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            look.IncludesHost = hostElement.GetBoolean();
        }

        if (lookElement.TryGetProperty("includesReader", out var readerElement) && readerElement.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            look.IncludesReader = readerElement.GetBoolean();
        }

        if (lookElement.TryGetProperty("plateTone", out var plateToneElement) && plateToneElement.ValueKind == JsonValueKind.String)
        {
            look.PlateTone = plateToneElement.GetString() ?? look.PlateTone;
        }

        if (lookElement.TryGetProperty("tallySource", out var tallySourceElement) && tallySourceElement.ValueKind == JsonValueKind.String)
        {
            look.TallySource = tallySourceElement.GetString() ?? look.TallySource;
        }

        if (lookElement.TryGetProperty("boxFill", out var boxFillElement) && boxFillElement.ValueKind == JsonValueKind.String)
        {
            look.BoxFill = boxFillElement.GetString() ?? look.BoxFill;
        }

        return look;
    }
}
