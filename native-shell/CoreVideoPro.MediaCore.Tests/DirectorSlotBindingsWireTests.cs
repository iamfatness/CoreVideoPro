using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class DirectorSlotBindingsWireTests
{
    [Fact]
    public void NativeRecommendationDeserializesPersonAndSourceForEachSlot()
    {
        const string json = """
            {"ruleId":"focused-interview","recommendedSceneId":"interview","confidence":92,
             "slotBindings":[{"slotIndex":0,"personId":"guest-7","sourceId":"zoom:guest-7"},
                             {"slotIndex":1,"personId":"guest-8","sourceId":"zoom:guest-8"}]}
            """;
        var recommendation = JsonSerializer.Deserialize<NativeMediaCoreAutoProduction>(
            json, new JsonSerializerOptions(JsonSerializerDefaults.Web));
        Assert.NotNull(recommendation);
        Assert.Equal(["guest-7", "guest-8"], recommendation.SlotBindings.Select(binding => binding.PersonId).ToArray());
        Assert.Equal(["zoom:guest-7", "zoom:guest-8"], recommendation.SlotBindings.Select(binding => binding.SourceId).ToArray());
    }
}
