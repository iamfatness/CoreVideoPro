using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 11 — pure text importer for the two legacy Isadora config files
/// (controller ruling D4). Synthetic legacy texts here are JS object literals (not JSON), with the
/// `"oscRole:"` trailing-colon typo and single-quoted strings the spec calls out
/// (`docs/superpowers/specs/2026-08-04-ohg-isadora-actor-reference.md` §0 H).</summary>
public sealed class IsadoraConfigImporterTests
{
    // §0 H shape, single-quoted (as the legacy files actually are), plus the "oscRole:" typo.
    private const string InfrastructureJsDoubleQuoted = """
        var infra = {"JohnZoomISO-1":{"address":"10.5.9.91","port":9090,"videoPins":8,"audioPins":8,"zak":"","manJoin":false,"videoRole":true,"oscRole:":true}};
        """;

    private const string InfrastructureJsSingleQuoted = """
        var infra = {'JohnZoomISO-1':{'address':'10.5.9.91','port':9090,'videoPins':8,'audioPins':8,'zak':'','manJoin':false,'videoRole':true,'oscRole:':true}};
        """;

    private const string MukanaJsDoubleQuoted = """
        var mukanaUrl = "https://hoka.pxclabs.com/phpsdk/php-panel-rest.php?event=officehours&req=panelists";
        """;

    private const string MukanaJsSingleQuoted = """
        var mukanaUrl = 'https://hoka.pxclabs.com/phpsdk/php-panel-rest.php?event=officehours&req=panelists';
        """;

    [Fact]
    public void DoubleQuotedLegacyText_FindsMukanaAndCapacityAndReportsFound()
    {
        var result = IsadoraConfigImporter.Import(InfrastructureJsDoubleQuoted, MukanaJsDoubleQuoted);

        Assert.Equal("https://hoka.pxclabs.com/phpsdk/php-panel-rest.php", result.Model.MukanaBaseUrl);
        Assert.Equal("officehours", result.Model.MukanaEvent);
        Assert.True(result.Model.RegistryEnabled);
        Assert.True(result.Model.HandsQueueEnabled);
        Assert.True(result.Model.QuestionFeedEnabled);

        Assert.Contains(result.Found, line => line.StartsWith("Mukana: https://hoka.pxclabs.com/phpsdk/php-panel-rest.php event=officehours", StringComparison.Ordinal));
        Assert.Contains(result.Found, line => line.Contains("legacy videoPins = 8", StringComparison.Ordinal));
        Assert.Contains(result.Found, line => line.Contains("capacity stays 10", StringComparison.Ordinal));
        Assert.Contains(result.Found, line => line.StartsWith("looks: wrote the 4 default looks", StringComparison.Ordinal));
    }

    [Fact]
    public void SingleQuotedLegacyText_StillFindsMukanaAndCapacity()
    {
        // Mutation-kill target: a regex that (wrongly) required double quotes only would red this.
        var result = IsadoraConfigImporter.Import(InfrastructureJsSingleQuoted, MukanaJsSingleQuoted);

        Assert.Equal("https://hoka.pxclabs.com/phpsdk/php-panel-rest.php", result.Model.MukanaBaseUrl);
        Assert.Equal("officehours", result.Model.MukanaEvent);
        Assert.Contains(result.Found, line => line.Contains("legacy videoPins = 8", StringComparison.Ordinal));
    }

    [Fact]
    public void CapacityIsNeverChanged_EvenWhenLegacyVideoPinsDiffers()
    {
        var result = IsadoraConfigImporter.Import(InfrastructureJsDoubleQuoted, MukanaJsDoubleQuoted);

        // Mutation-kill target: an importer that clamped/overwrote Capacity from videoPins would
        // red this (legacy videoPins is 8; capacity must stay the Default() 10).
        Assert.Equal(10, result.Model.Capacity);
    }

    [Fact]
    public void NullOrEmptyInputs_EveryProbeReportsNotFound_AndModelMatchesDefault()
    {
        var result = IsadoraConfigImporter.Import(null, null);

        Assert.Contains("Mukana REST URL (php-panel-rest.php?event=…) not found", result.NotFound);
        Assert.Contains("ZoomISO videoPins not found", result.NotFound);
        Assert.Contains("tally URL not found", result.NotFound);
        Assert.DoesNotContain(result.Found, line => line.StartsWith("Mukana:", StringComparison.Ordinal));

        var expected = OhgConfigEditModel.Default();
        Assert.Equal(expected.Capacity, result.Model.Capacity);
        Assert.Equal(expected.MukanaBaseUrl, result.Model.MukanaBaseUrl);
        Assert.Equal(expected.MukanaEvent, result.Model.MukanaEvent);
        Assert.Equal(expected.RegistryEnabled, result.Model.RegistryEnabled);
        Assert.Equal(expected.HandsQueueEnabled, result.Model.HandsQueueEnabled);
        Assert.Equal(expected.QuestionFeedEnabled, result.Model.QuestionFeedEnabled);
        Assert.Equal(expected.TallyUrl, result.Model.TallyUrl);
        Assert.Equal(expected.Looks.Count, result.Model.Looks.Count);
        for (var i = 0; i < expected.Looks.Count; i++)
        {
            Assert.Equal(expected.Looks[i].Id, result.Model.Looks[i].Id);
            Assert.Equal(expected.Looks[i].Label, result.Model.Looks[i].Label);
            Assert.Equal(expected.Looks[i].Boxes, result.Model.Looks[i].Boxes);
        }
    }

    [Fact]
    public void EmptyStringInputs_AlsoAllNotFound_NeverThrows()
    {
        var result = IsadoraConfigImporter.Import("", "");

        Assert.Contains("Mukana REST URL (php-panel-rest.php?event=…) not found", result.NotFound);
        Assert.Contains("ZoomISO videoPins not found", result.NotFound);
        Assert.Contains("tally URL not found", result.NotFound);
    }

    [Fact]
    public void ExistingLooksArePreserved_NotOverwrittenByDefaults()
    {
        // Mutation-kill target: an importer that overwrote existing.Looks unconditionally would
        // red this.
        var existing = OhgConfigEditModel.Default();
        existing.Looks =
        [
            new OhgLookEdit { Id = "custom-1", Label = "Custom One", Boxes = 2 },
        ];

        var result = IsadoraConfigImporter.Import(InfrastructureJsDoubleQuoted, MukanaJsDoubleQuoted, existing);

        Assert.Single(result.Model.Looks);
        Assert.Equal("custom-1", result.Model.Looks[0].Id);
        Assert.Equal("Custom One", result.Model.Looks[0].Label);
        Assert.Contains(result.Found, line => line.StartsWith("looks: kept 1 existing", StringComparison.Ordinal));
    }

    [Fact]
    public void CallersExistingModel_IsNeverMutated()
    {
        var existing = OhgConfigEditModel.Default();
        existing.Looks =
        [
            new OhgLookEdit { Id = "custom-1", Label = "Custom One", Boxes = 2 },
        ];
        existing.MukanaBaseUrl = null;
        existing.MukanaEvent = null;
        existing.Capacity = 10;

        _ = IsadoraConfigImporter.Import(InfrastructureJsDoubleQuoted, MukanaJsDoubleQuoted, existing);

        Assert.Null(existing.MukanaBaseUrl);
        Assert.Null(existing.MukanaEvent);
        Assert.False(existing.RegistryEnabled);
        Assert.False(existing.HandsQueueEnabled);
        Assert.False(existing.QuestionFeedEnabled);
        Assert.Single(existing.Looks);
        Assert.Equal("custom-1", existing.Looks[0].Id);
    }

    [Fact]
    public void TallyUrlLiteral_IsFoundAndSet()
    {
        const string infra = """var infra = {"tallyLine":"https://oh.tally.example.com/api/1"};""";
        var result = IsadoraConfigImporter.Import(infra, null);

        Assert.Equal("https://oh.tally.example.com/api/1", result.Model.TallyUrl);
        Assert.Contains(result.Found, line => line.StartsWith("tally URL:", StringComparison.Ordinal));
    }

    [Fact]
    public void TallyUrlField_IsFoundAndSet()
    {
        const string infra = """var infra = {"tallyUrl": "https://not-matching-literal.example.com/x"};""";
        var result = IsadoraConfigImporter.Import(infra, null);

        Assert.Equal("https://not-matching-literal.example.com/x", result.Model.TallyUrl);
    }
}
