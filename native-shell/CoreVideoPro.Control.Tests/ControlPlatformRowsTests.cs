using System.Text.Json;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlPlatformRowsTests
{
    private static JsonElement Platforms() =>
        JsonDocument.Parse(File.ReadAllText(RepoPaths.Contract("control.platforms.json"))).RootElement.Clone();

    private static readonly string[] Values = ["supported", "blocked", "notApplicable"];

    [Fact]
    public void EveryRegistryActionHasExactlyOneMacRowInRegistryOrder()
    {
        var ids = Platforms().GetProperty("actions").EnumerateArray().Select(r => r.GetProperty("id").GetString()!).ToList();
        Assert.Equal(ids.Count, ids.Distinct(StringComparer.Ordinal).Count());
        var missing = ControlActionRegistry.Actions.Select(a => a.Id).Except(ids, StringComparer.Ordinal).ToList();
        var unknown = ids.Except(ControlActionRegistry.Actions.Select(a => a.Id), StringComparer.Ordinal).ToList();
        Assert.True(missing.Count == 0, "A Windows action has no macOS row; add one to contracts/control.platforms.json: " + string.Join(", ", missing));
        Assert.True(unknown.Count == 0, "A macOS row names no registry action: " + string.Join(", ", unknown));
        Assert.Equal(ControlActionRegistry.Actions.Select(a => a.Id), ids);
    }

    [Fact]
    public void EveryFeedbackFieldHasExactlyOneMacRowInStateFieldsOrder()
    {
        var fields = Platforms().GetProperty("feedbackFields").EnumerateArray().Select(r => r.GetProperty("field").GetString()!).ToList();
        Assert.Equal(ControlManifest.StateFields, fields);
    }

    [Fact]
    public void RowsAreWellFormed()
    {
        var root = Platforms();
        var workstreams = root.GetProperty("workstreams").EnumerateObject().Select(p => p.Name).ToHashSet(StringComparer.Ordinal);
        Assert.All(root.GetProperty("workstreams").EnumerateObject(), p => Assert.False(string.IsNullOrWhiteSpace(p.Value.GetString())));
        foreach (var row in root.GetProperty("actions").EnumerateArray().Concat(root.GetProperty("feedbackFields").EnumerateArray()))
        {
            var name = row.TryGetProperty("id", out var id) ? id.GetString() : row.GetProperty("field").GetString();
            var mac = row.GetProperty("mac").GetString();
            Assert.True(Values.Contains(mac), $"{name}: mac must be one of {string.Join("|", Values)}");
            var hasBlockedOn = row.TryGetProperty("blockedOn", out var blockedOn);
            var hasReason = row.TryGetProperty("reason", out var reason);
            switch (mac)
            {
                case "supported":
                    Assert.False(hasBlockedOn || hasReason, $"{name}: a supported row carries no blockedOn or reason");
                    break;
                case "blocked":
                    Assert.True(hasBlockedOn && workstreams.Contains(blockedOn.GetString()!), $"{name}: blockedOn must name a workstream");
                    Assert.False(hasReason, $"{name}: a blocked row's sentence comes from its workstream label");
                    break;
                default:
                    Assert.True(hasReason && !string.IsNullOrWhiteSpace(reason.GetString()), $"{name}: notApplicable needs a reason");
                    Assert.False(hasBlockedOn, $"{name}: notApplicable has no blockedOn");
                    break;
            }
        }
    }
}
