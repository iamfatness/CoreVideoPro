using System.Text.RegularExpressions;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Final-review guard for the three look ENUMERATIONS the settings section offers and the Save-time
/// rules that back them (Plan 7b final fix wave).
///
/// <para><b>The defect this exists for.</b> The plate-tone picker offered
/// <c>neutral | warm | cool</c>. The engine's enumeration is
/// <c>neutral | accent | guest | breaking</c>, and <c>optionalPlateTone</c>
/// (<c>show-engine/src/config.ts</c>) THROWS on anything else — so saving a look with "warm" wrote
/// a config that made the host exit <b>78</b>: config rejected, which the supervisor treats as
/// TERMINAL (no backoff, no respawn). A picker offering an illegal value is therefore a show-killer,
/// not a cosmetic bug, and text-matching the arrays against literal copies of the engine's is the
/// cheapest tripwire that keeps them in step.</para>
///
/// <para>The copies below are hand-typed from <c>show-engine/src/contracts.ts</c>
/// (<c>PLATE_TONES</c>, <c>TALLY_SOURCES</c>, <c>BOX_FILLS</c>) — the same
/// hand-copied-contract-table technique <c>OhgActionArgsTests</c> uses. A second assertion reads
/// the TypeScript file itself, so a change on the engine side fails here even if nobody retypes
/// the literals.</para>
/// </summary>
public sealed class OhgSettingsChoicesTests : IDisposable
{
    // Literal copies of the engine's arrays.
    private static readonly string[] EnginePlateTones = ["neutral", "accent", "guest", "breaking"];
    private static readonly string[] EngineTallySources = ["boxes", "activeSpeaker"];
    private static readonly string[] EngineBoxFills = ["queue", "manual"];

    [Fact]
    public void ThePickersOfferExactlyTheEnginesEnumerations()
    {
        Assert.Equal(EnginePlateTones, ProductionSettingsWindow.PlateTones);
        Assert.Equal(EngineTallySources, ProductionSettingsWindow.TallySources);
        Assert.Equal(EngineBoxFills, ProductionSettingsWindow.BoxFills);

        // ...and the pickers read the ONE shared copy the validator reads.
        Assert.Same(OhgLookChoices.PlateTones, ProductionSettingsWindow.PlateTones);
        Assert.Same(OhgLookChoices.TallySources, ProductionSettingsWindow.TallySources);
        Assert.Same(OhgLookChoices.BoxFills, ProductionSettingsWindow.BoxFills);
    }

    /// <summary>The other half of the tripwire: the literals above are checked against the engine's
    /// actual source, so drift fails here rather than at exit 78 on a show night.</summary>
    [Fact]
    public void TheLiteralCopiesStillMatchTheEnginesContractsFile()
    {
        var contracts = ReadEngineSource("contracts.ts");

        Assert.Equal(EnginePlateTones, ReadStringArray(contracts, "PLATE_TONES"));
        Assert.Equal(EngineTallySources, ReadStringArray(contracts, "TALLY_SOURCES"));
        Assert.Equal(EngineBoxFills, ReadStringArray(contracts, "BOX_FILLS"));
    }

    // ── the same sets, enforced at Save (a hand-edited config must not slip through) ──

    [Fact]
    public void Validate_RefusesAPlateToneTheEngineWouldThrowOn()
        => AssertRefusedAndNotSaved(
            look => look.PlateTone = "warm",
            "plateTone must be one of neutral, accent, guest, breaking");

    [Fact]
    public void Validate_RefusesATallySourceTheEngineWouldThrowOn()
        => AssertRefusedAndNotSaved(
            look => look.TallySource = "program",
            "tallySource must be one of boxes, activeSpeaker");

    [Fact]
    public void Validate_RefusesABoxFillTheEngineWouldThrowOn()
        => AssertRefusedAndNotSaved(
            look => look.BoxFill = "auto",
            "boxFill must be one of queue, manual");

    [Fact]
    public void Validate_AcceptsEveryLegalValueOfEachEnumeration()
    {
        foreach (var tone in EnginePlateTones)
        {
            foreach (var tally in EngineTallySources)
            {
                foreach (var fill in EngineBoxFills)
                {
                    var vm = MakeViewModel(out _);
                    Ready(vm);
                    foreach (var look in vm.Model.Looks)
                    {
                        look.PlateTone = tone;
                        look.TallySource = tally;
                        look.BoxFill = fill;
                    }

                    Assert.Null(vm.Validate());
                }
            }
        }
    }

    // ── I2: a look's LABEL is required, and a blank id is caught even when it is whitespace ──

    [Fact]
    public void Validate_RefusesALookWithNoLabel_BecauseTheEngineRefusesAnEmptyString()
        => AssertRefusedAndNotSaved(look => look.Label = "   ", "needs a label");

    [Fact]
    public void Validate_RefusesAWhitespaceOnlyLookId()
        => AssertRefusedAndNotSaved(look => look.Id = "   ", "Every look needs an id");

    /// <summary>The exact shape the "Add look" button produces — <c>{Id="", Label=""}</c>. It used
    /// to pass validation and reach the engine as <c>label: ""</c>, which <c>requireString</c>
    /// refuses at parse time: exit 78, terminal.</summary>
    [Fact]
    public async Task AFreshlyAddedLookIsRefusedAtSaveInsteadOfKillingTheEngine()
    {
        var vm = MakeViewModel(out var applied);
        Ready(vm);
        vm.AddLookCommand.Execute(null);

        await vm.SaveCommand.ExecuteAsync(null);

        Assert.Equal("Every look needs an id", vm.ValidationMessage);
        Assert.Empty(applied);
        Assert.False(new ShowConfigStore(_dir).Exists);
    }

    // ── rig ──

    private readonly string _dir;

    public OhgSettingsChoicesTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cvp-ohg-choices-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static readonly IReadOnlySet<string> Scenes =
        new HashSet<string>(StringComparer.Ordinal) { "scene-a", "scene-b", "scene-c", "scene-d" };

    private static readonly IReadOnlyList<(string Id, string Name)> SceneList =
    [
        ("scene-a", "Scene A"),
        ("scene-b", "Scene B"),
        ("scene-c", "Scene C"),
        ("scene-d", "Scene D")
    ];

    private OhgSettingsViewModel MakeViewModel(out List<ShowConfig> applied)
    {
        var appliedConfigs = new List<ShowConfig>();
        applied = appliedConfigs;

        return new OhgSettingsViewModel(
            new ShowConfigStore(_dir),
            () => Scenes,
            () => SceneList,
            cfg =>
            {
                appliedConfigs.Add(cfg);
                return Task.FromResult<string?>(null);
            },
            engineStartedAtLaunch: true);
    }

    /// <summary>A model that validates clean, so the ONLY thing a test's mutation can break is the
    /// rule it is about.</summary>
    private static void Ready(OhgSettingsViewModel vm)
    {
        vm.Model.PresetSolo = "scene-a";
        vm.Model.PresetActiveSpeaker = "scene-b";
        vm.Model.PresetBlack = "scene-c";
        vm.Model.PresetGallery = "scene-d";
        foreach (var look in vm.Model.Looks)
        {
            look.ScenePreset = "scene-a";
        }

        Assert.Null(vm.Validate());
    }

    /// <summary>Applies <paramref name="mutate"/> to the first look, then asserts Save REFUSES with
    /// a message containing <paramref name="expected"/> and writes NOTHING — neither the config file
    /// nor an apply (which would restart the engine onto the bad config).</summary>
    private void AssertRefusedAndNotSaved(Action<OhgLookEdit> mutate, string expected)
    {
        var vm = MakeViewModel(out var applied);
        Ready(vm);
        mutate(vm.Model.Looks[0]);

        var problem = vm.Validate();
        Assert.NotNull(problem);
        Assert.Contains(expected, problem, StringComparison.Ordinal);

        vm.SaveCommand.ExecuteAsync(null).GetAwaiter().GetResult();

        Assert.Equal(problem, vm.ValidationMessage);
        Assert.Empty(applied);
        Assert.False(new ShowConfigStore(_dir).Exists);
    }

    private static string[] ReadStringArray(string source, string constantName)
    {
        var match = Regex.Match(source, constantName + @"[^=]*=\s*\[(?<body>[^\]]*)\]");
        Assert.True(match.Success, $"Could not find {constantName} in the engine's contracts.ts.");

        return Regex.Matches(match.Groups["body"].Value, "\"(?<value>[^\"]+)\"")
            .Select(m => m.Groups["value"].Value)
            .ToArray();
    }

    private static string ReadEngineSource(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "show-engine", "src", fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate show-engine/src/{fileName}.");
    }
}
