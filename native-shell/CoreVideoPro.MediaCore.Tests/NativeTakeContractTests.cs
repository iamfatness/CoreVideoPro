using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeTakeContractTests
{
    private static NativeTakePreparationToken Token(long revision = 10) => new(
        new("show", revision, 7), "plan", revision + 1, 3,
        new("show", "registry", revision, 12, "eligibility-v1"));
    private static NativeTakeRequest Request(string id = "caller-stable-id") => new("show", id,
        new(10, 8, "media-process", 7, NativeTakeTransition.FromLegacy("fade"), Token()));

    [Fact]
    public void RetrySerializationRetainsCallerOperationAndFullFingerprint()
    {
        var request = Request(); var first = request.Serialize();
        Assert.Equal(first, request.Serialize());
        using var document = JsonDocument.Parse(first); var root = document.RootElement;
        Assert.Equal("take", root.GetProperty("type").GetString());
        Assert.Equal("caller-stable-id", root.GetProperty("operationId").GetString());
        var fingerprint = root.GetProperty("fingerprint");
        Assert.Equal(10, fingerprint.GetProperty("expectedRevision").GetInt64());
        Assert.Equal(8, fingerprint.GetProperty("previewRevision").GetInt64());
        Assert.Equal("media-process", fingerprint.GetProperty("mediaProcessEpoch").GetString());
        Assert.Equal(300_000_000, fingerprint.GetProperty("transition").GetProperty("durationNs").GetInt64());
        Assert.Equal(fingerprint.GetProperty("expectedPlanStamp").GetRawText(),
            fingerprint.GetProperty("preparation").GetProperty("stamp").GetRawText());
        Assert.Equal(11, fingerprint.GetProperty("preparation").GetProperty("planRevision").GetInt64());
        Assert.False(first.Contains("certificate", StringComparison.OrdinalIgnoreCase));
        Assert.NotEqual(first, Request("another-id").Serialize());
    }

    [Fact]
    public void NativeIssuedNestedRequestDeserializesWithoutChangingItsFingerprint()
    {
        var original = Request().Serialize();
        var decoded = JsonSerializer.Deserialize<NativeTakeRequest>(original)!;
        Assert.Equal("caller-stable-id", decoded.OperationId);
        Assert.Equal(original, decoded.Serialize());
        var lower = new NativeTakeTransition("dip", 300_000_000, dipColor: "#abcdef");
        Assert.Equal("#abcdef", lower.DipColor);
    }

    [Theory]
    [InlineData("cut", 0, "", "")]
    [InlineData("fade", 300000000, "", "")]
    [InlineData("dip", 300000000, "", "#ABCDEF")]
    [InlineData("wipe", 300000000, "right-to-left", "")]
    public void LegacyDefaultsAreCanonicalizedWithoutIrrelevantFields(string mode, long ns, string direction, string color)
    {
        var result = NativeTakeTransition.FromLegacy(mode, 300, "right-to-left", "#abcdef");
        Assert.Equal(ns, result.DurationNs); Assert.Equal(direction, result.Direction); Assert.Equal(color, result.DipColor);
    }

    [Theory]
    [InlineData("unknown", 300, "left-to-right", "#000000")]
    [InlineData("fade", 0, "left-to-right", "#000000")]
    [InlineData("fade", 5001, "left-to-right", "#000000")]
    [InlineData("wipe", 300, "diagonal", "#000000")]
    [InlineData("dip", 300, "left-to-right", "#GG0000")]
    public void InvalidTransitionNeverSilentlyFallsBack(string mode, int ms, string direction, string color) =>
        Assert.Throws<ArgumentException>(() => NativeTakeTransition.FromLegacy(mode, ms, direction, color));

    [Fact]
    public void DirectConstructionRejectsNonCanonicalTransitionPayload()
    {
        Assert.Throws<ArgumentException>(() => new NativeTakeTransition("cut", 300_000_000));
        Assert.Throws<ArgumentException>(() => new NativeTakeTransition("fade", 1, "left-to-right"));
        Assert.Throws<ArgumentException>(() => new NativeTakeTransition("wipe", 1, "left-to-right", "#000000"));
    }

    [Fact]
    public void RevisionsAndEpochsMustMatchPreparationAndSafeIntegerRange()
    {
        const long max = 9_007_199_254_740_991;
        Assert.Equal(max, Token(max - 1).PlanRevision);
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeTakePreparationBase("show", max + 1, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeTakePreparationBase("show", -1, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => new NativeTakePreparationBase("show", 1, 0));
        Assert.Throws<ArgumentException>(() => new NativeTakeFingerprint(9, 8, "media", 7, new("cut", 0), Token()));
        Assert.Throws<ArgumentException>(() => new NativeTakeRequest("wrong-epoch", "id", Request().Fingerprint));
        Assert.Throws<ArgumentException>(() => Request(""));
        Assert.Throws<ArgumentException>(() => Request(new string('é', 257)));
    }

    [Fact]
    public void OutcomeDoesNotManufactureDownstreamEvidence()
    {
        var pending = new NativeTakeOutcome("show", "id", "none", true, true, false, false, false, 11, "");
        Assert.False(pending.Applied); Assert.False(pending.Rendered); Assert.False(pending.Delivered);
        var serialized = JsonSerializer.Serialize(pending);
        var decoded = JsonSerializer.Deserialize<NativeTakeOutcome>(serialized)!;
        Assert.True(decoded.Pending); Assert.Equal("id", decoded.OperationId);
        Assert.Throws<ArgumentException>(() => new NativeTakeOutcome("show", "id", "none", false, true, false, false, true, 11, ""));
        Assert.Throws<ArgumentException>(() => new NativeTakeOutcome("show", "id", "applyFailed", false, true, true, false, false, 10, "failed"));
        Assert.Throws<ArgumentException>(() => new NativeTakeOutcome("show", "id", "none", true, true, true, false, false, 11, ""));
    }
}
