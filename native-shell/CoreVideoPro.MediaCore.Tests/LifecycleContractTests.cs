using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class LifecycleContractTests
{
    private static string FixturesPath()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(directory.FullName, "contracts", "lifecycle.fixtures.json");
            if (File.Exists(path)) return path;
            directory = directory.Parent;
        }
        throw new FileNotFoundException("Shared lifecycle fixtures must be available from repository root.");
    }

    public static IEnumerable<object[]> Fixtures()
    {
        using var file = JsonDocument.Parse(File.ReadAllText(FixturesPath()));
        return file.RootElement.EnumerateArray().Select(item => new object[] {
            item.GetProperty("id").GetString()!, item.GetProperty("contract").GetString()!,
            item.GetProperty("accepted").GetBoolean(), item.GetProperty("json").GetString()!
        }).ToArray();
    }

    [Theory]
    [MemberData(nameof(Fixtures))]
    public void GoldenMessagesValidateAndValidModelsRoundTrip(string id, string contract, bool accepted, string json)
    {
        using var document = JsonDocument.Parse(json);
        Func<JsonElement, bool> validate = contract switch
        {
            "ProtocolVersion" => ProtocolVersionContract.Validate,
            "OutputLifecycle" => OutputLifecycleContract.Validate,
            "OperationStatus" => OperationStatusContract.Validate,
            "ProtocolFailure" => ProtocolFailureContract.Validate,
            _ => throw new ArgumentException(contract)
        };
        Assert.True(validate(document.RootElement) == accepted, id);
        if (!accepted) return;
        var type = contract switch
        {
            "ProtocolVersion" => typeof(ProtocolVersion), "OutputLifecycle" => typeof(OutputLifecycle),
            "OperationStatus" => typeof(OperationStatus), _ => typeof(ProtocolFailure)
        };
        var model = JsonSerializer.Deserialize(json, type);
        using var roundTrip = JsonDocument.Parse(JsonSerializer.Serialize(model, type));
        Assert.True(validate(roundTrip.RootElement), id + " round trip");
    }
}
