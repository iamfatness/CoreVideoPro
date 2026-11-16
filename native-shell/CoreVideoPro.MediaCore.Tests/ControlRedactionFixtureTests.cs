using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>The redaction block of contracts/control.fixtures.json, run through the Windows
/// observer boundary (CoreSnapshotObserver / SupportBundleLogRedactor). On Windows the router serves
/// JSON that is already redacted here (spec correction 2); the Mac router redacts at its own boundary
/// and runs the same block in control/fixtures.</summary>
public sealed class ControlRedactionFixtureTests
{
    private static JsonObject Redaction() =>
        JsonNode.Parse(File.ReadAllText(ControlRepoPaths.Contract("control.fixtures.json")))!["redaction"]!.AsObject();

    public static IEnumerable<object[]> Names() =>
        Redaction()["names"]!.AsArray().Select(n => new object[] { n!["name"]!.GetValue<string>(), n["secret"]!.GetValue<bool>() });

    public static IEnumerable<object[]> Texts() =>
        Redaction()["texts"]!.AsArray().Select(n => new object[] { n!["input"]!.GetValue<string>(), n["expected"]!.GetValue<string>() });

    public static IEnumerable<object[]> Documents() =>
        Redaction()["documents"]!.AsArray().Select((n, i) => new object[] { i });

    [Fact]
    public void EveryRedactionFixtureCaseIsRun()
    {
        Assert.Equal(21, Names().Count());
        Assert.Equal(11, Texts().Count());
        Assert.Single(Documents());
    }

    [Theory]
    [MemberData(nameof(Names))]
    public void SecretNames(string name, bool secret) => Assert.Equal(secret, CoreSnapshotObserver.IsSecretName(name));

    [Theory]
    [MemberData(nameof(Texts))]
    public void Texts_(string input, string expected) => Assert.Equal(expected, SupportBundleLogRedactor.Redact(input));

    [Theory]
    [MemberData(nameof(Documents))]
    public void Document(int index)
    {
        var pair = Redaction()["documents"]!.AsArray()[index]!;
        var redacted = JsonNode.Parse(CoreSnapshotObserver.Redact(pair["input"]!.ToJsonString()));
        Assert.True(JsonNode.DeepEquals(pair["expected"], redacted), $"redacted document: {redacted?.ToJsonString()}");
    }
}
