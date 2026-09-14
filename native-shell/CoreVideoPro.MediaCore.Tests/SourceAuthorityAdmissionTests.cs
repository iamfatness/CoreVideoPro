using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class SourceAuthorityAdmissionTests
{
    private static NativeSourceAuthority Catalog(string epoch = "epoch-a", long sequence = 1, bool available = true) => new()
    {
        Version = 1, Valid = true, ProcessEpoch = epoch, Sequence = sequence,
        Sources = [new() { SourceId = "camera", InstanceId = epoch + ":camera", ProcessEpoch = epoch,
            Generation = 7, ParticipantId = "42", Kind = "camera", Available = available }]
    };
    private static void Rejected(NativeSourceAuthority? value)
    {
        Assert.NotNull(value); Assert.False(value.Valid); Assert.Empty(value.Sources!);
    }

    [Fact]
    public void SharedPublicationsRejectOlderSequenceAndPreserveExactReplay()
    {
        var gate = new SourceAuthorityAdmission();
        var capture = gate.Admit(0, Catalog(sequence: 10));
        Assert.True(capture!.Valid);
        Assert.Same(capture, gate.Admit(0, Catalog(sequence: 10)));
        Rejected(gate.Admit(0, Catalog(sequence: 9)));
        Assert.True(gate.Admit(0, Catalog(sequence: 11))!.Valid);
        Rejected(gate.Admit(0, Catalog(sequence: 10)));
    }

    [Fact]
    public void SameSequenceChangedPayloadIsRejectedWithoutReplacingAcceptedEvidence()
    {
        var gate = new SourceAuthorityAdmission(); var original = gate.Admit(3, Catalog());
        Rejected(gate.Admit(3, Catalog(available: false)));
        var changedToken = Catalog() with { Sources = [Catalog().Sources![0] with { Generation = 8 }] };
        Rejected(gate.Admit(3, changedToken));
        Assert.Same(original, gate.Admit(3, Catalog()));
    }

    [Fact]
    public void RetiredEpochCannotReturnDespiteHigherSequence()
    {
        var gate = new SourceAuthorityAdmission();
        Assert.True(gate.Admit(4, Catalog())!.Valid);
        Assert.True(gate.Admit(4, Catalog("epoch-b"))!.Valid);
        Rejected(gate.Admit(4, Catalog("epoch-a", 999)));
        Assert.True(gate.Admit(4, Catalog("epoch-b", 2))!.Valid);
    }

    [Fact]
    public void CapturedProcessGenerationFencesDelayedPreviousProcessResponses()
    {
        var gate = new SourceAuthorityAdmission(); gate.Admit(8, Catalog());
        Assert.True(gate.Admit(9, Catalog("new-process"))!.Valid);
        Rejected(gate.Admit(8, Catalog("old-but-unseen-epoch", 999)));
        Rejected(gate.Admit(-1, Catalog()));
        Assert.True(gate.Admit(9, Catalog("new-process", 2))!.Valid);
    }

    [Fact]
    public void InvalidAndNullEvidenceNeverExposePriorCatalog()
    {
        var gate = new SourceAuthorityAdmission(); gate.Admit(1, Catalog(sequence: 10));
        Assert.Null(gate.Admit(1, null));
        Rejected(gate.Admit(1, Catalog(sequence: 20) with { Valid = false }));
        Rejected(gate.Admit(1, Catalog(sequence: 20) with { Sources = null }));
        Assert.True(gate.Admit(1, Catalog(sequence: 11))!.Valid);
        Assert.Null(gate.Admit(2, null));
        Rejected(gate.Admit(1, Catalog(sequence: 100)));
        Assert.Null(gate.Admit(1, null));
        Assert.True(gate.Admit(2, Catalog("new"))!.Valid);
    }

    [Fact]
    public void PublicationAdmissionRejectsOrderedStaleSnapshotsButKeepsCurrentMalformedStatePublishable()
    {
        var gate = new SourceAuthorityAdmission();
        Assert.True(gate.TryAdmit(5, Catalog(sequence: 10), out var current));
        Assert.True(current!.Valid);
        Assert.False(gate.TryAdmit(5, Catalog(sequence: 9), out var lower));
        Rejected(lower);
        Assert.False(gate.TryAdmit(4, null, out var oldProcess));
        Assert.Null(oldProcess);
        Assert.True(gate.TryAdmit(5, Catalog(sequence: 11) with { Valid = false }, out var malformed));
        Rejected(malformed);
        Assert.True(gate.TryAdmit(5, Catalog(sequence: 11), out var recovered));
        Assert.True(recovered!.Valid);
    }

    [Fact]
    public void EpochHistoryIsBoundedWithoutEvictingSafetyFences()
    {
        var gate = new SourceAuthorityAdmission(1);
        gate.Admit(0, Catalog("a")); gate.Admit(0, Catalog("b"));
        Rejected(gate.Admit(0, Catalog("c")));
        Rejected(gate.Admit(0, Catalog("a", 999)));
        Assert.True(gate.Admit(0, Catalog("b", 2))!.Valid);
        Assert.True(gate.Admit(1, Catalog("c"))!.Valid);
        Assert.Throws<ArgumentOutOfRangeException>(() => new SourceAuthorityAdmission(0));
        Assert.Throws<ArgumentOutOfRangeException>(() => new SourceAuthorityAdmission(65_537));
    }

    [Fact]
    public void AdmissionFreezesCallerOwnedSourceList()
    {
        var rows = Catalog().Sources!.ToList();
        var input = Catalog() with { Sources = rows };
        var gate = new SourceAuthorityAdmission(); var result = gate.Admit(0, input)!;
        rows.Clear();
        Assert.Single(result.Sources!);
        Assert.True(gate.Admit(0, Catalog())!.Valid);
    }

    [Fact]
    public async Task RacingPublicationsCannotRegressAcceptedWatermark()
    {
        var gate = new SourceAuthorityAdmission();
        await Task.WhenAll(Enumerable.Range(1, 100).Select(sequence => Task.Run(() => gate.Admit(1, Catalog(sequence: sequence)))));
        Assert.True(gate.Admit(1, Catalog(sequence: 100))!.Valid);
        Rejected(gate.Admit(1, Catalog(sequence: 99)));
    }
}
