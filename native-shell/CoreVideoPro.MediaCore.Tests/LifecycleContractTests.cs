using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class LifecycleContractTests
{
    private static string FixturesPath(string name)
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            var path = Path.Combine(directory.FullName, "contracts", name);
            if (File.Exists(path)) return path;
            directory = directory.Parent;
        }
        throw new FileNotFoundException("Shared lifecycle fixtures must be available from repository root.");
    }

    public static IEnumerable<object[]> Fixtures()
    {
        foreach (var name in new[] { "lifecycle.fixtures.json", "identity.fixtures.json", "evidence.fixtures.json" })
        {
            using var file = JsonDocument.Parse(File.ReadAllText(FixturesPath(name)));
            foreach (var item in file.RootElement.EnumerateArray()) yield return new object[] {
                item.GetProperty("id").GetString()!, item.GetProperty("contract").GetString()!,
                item.GetProperty("accepted").GetBoolean(), item.GetProperty("json").GetString()!
        };
        }
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
            "EntityIdentity" => EntityIdentityContract.Validate,
            "EntityRevision" => EntityRevisionContract.Validate,
            "SourceInstanceIdentity" => SourceInstanceIdentityContract.Validate,
            "ParticipantBindingIdentity" => ParticipantBindingIdentityContract.Validate,
            "ControlRevision" => ControlRevisionContract.Validate,
            "PlanGeneration" => PlanGenerationContract.Validate,
            "ControlOperationIdentity" => ControlOperationIdentityContract.Validate,
            "AcceptedOperationObservation" => AcceptedOperationObservationContract.Validate,
            "AppliedOperationObservation" => AppliedOperationObservationContract.Validate,
            "RenderedMediaObservation" => RenderedMediaObservationContract.Validate,
            "DeliveredMediaObservation" => DeliveredMediaObservationContract.Validate,
            "PresentedMediaObservation" => PresentedMediaObservationContract.Validate,
            "MuxedMediaObservation" => MuxedMediaObservationContract.Validate,
            "CommittedMediaObservation" => CommittedMediaObservationContract.Validate,
            "CompletedOutputObservation" => CompletedOutputObservationContract.Validate,
            "ResourceLeaseDescriptor" => ResourceLeaseDescriptorContract.Validate,
            "DestinationProgress" => DestinationProgressContract.Validate,
            "ArtifactValidationResult" => ArtifactValidationResultContract.Validate,
            _ => throw new ArgumentException(contract)
        };
        Assert.True(validate(document.RootElement) == accepted, id);
        if (!accepted) return;
        var type = contract switch
        {
            "ProtocolVersion" => typeof(ProtocolVersion), "OutputLifecycle" => typeof(OutputLifecycle),
            "OperationStatus" => typeof(OperationStatus), "ProtocolFailure" => typeof(ProtocolFailure),
            "EntityIdentity" => typeof(EntityIdentity),
            "EntityRevision" => typeof(EntityRevision),
            "SourceInstanceIdentity" => typeof(SourceInstanceIdentity),
            "ParticipantBindingIdentity" => typeof(ParticipantBindingIdentity),
            "ControlRevision" => typeof(ControlRevision),
            "PlanGeneration" => typeof(PlanGeneration),
            "ControlOperationIdentity" => typeof(ControlOperationIdentity),
            "AcceptedOperationObservation" => typeof(AcceptedOperationObservation),
            "AppliedOperationObservation" => typeof(AppliedOperationObservation),
            "RenderedMediaObservation" => typeof(RenderedMediaObservation),
            "DeliveredMediaObservation" => typeof(DeliveredMediaObservation),
            "PresentedMediaObservation" => typeof(PresentedMediaObservation),
            "MuxedMediaObservation" => typeof(MuxedMediaObservation),
            "CommittedMediaObservation" => typeof(CommittedMediaObservation),
            "CompletedOutputObservation" => typeof(CompletedOutputObservation),
            "ResourceLeaseDescriptor" => typeof(ResourceLeaseDescriptor),
            "DestinationProgress" => typeof(DestinationProgress),
            "ArtifactValidationResult" => typeof(ArtifactValidationResult),
            _ => throw new ArgumentException(contract)
        };
        var model = JsonSerializer.Deserialize(json, type);
        using var roundTrip = JsonDocument.Parse(JsonSerializer.Serialize(model, type));
        Assert.True(validate(roundTrip.RootElement), id + " round trip");
    }
}
