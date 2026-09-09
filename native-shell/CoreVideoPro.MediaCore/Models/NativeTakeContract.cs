using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

internal static class NativeTakeValidation
{
    internal const long MaxSafeInteger = 9_007_199_254_740_991;
    internal static long Revision(long value, string name, bool positive = false)
    {
        if (value < (positive ? 1 : 0) || value > MaxSafeInteger) throw new ArgumentOutOfRangeException(name);
        return value;
    }
    internal static string Text(string value, string name, int maxBytes = 512)
    {
        ArgumentNullException.ThrowIfNull(value, name);
        if (value.Length == 0 || new UTF8Encoding(false, true).GetByteCount(value) > maxBytes)
            throw new ArgumentException("A bounded, nonempty UTF-8 value is required.", name);
        return value;
    }
}

public sealed class NativeTakeTransition
{
    [JsonPropertyName("kind")] public string Kind { get; }
    [JsonPropertyName("durationNs")] public long DurationNs { get; }
    [JsonPropertyName("direction")] public string Direction { get; }
    [JsonPropertyName("dipColor")] public string DipColor { get; }
    public NativeTakeTransition(string kind, long durationNs, string direction = "", string dipColor = "")
    {
        var valid = kind switch
        {
            "cut" => durationNs == 0 && direction == "" && dipColor == "",
            "fade" => Timed(durationNs) && direction == "" && dipColor == "",
            "dip" => Timed(durationNs) && direction == "" && IsColor(dipColor),
            "wipe" => Timed(durationNs) && direction is "left-to-right" or "right-to-left" or "top-to-bottom" or "bottom-to-top" && dipColor == "",
            _ => false
        };
        if (!valid) throw new ArgumentException("Transition is not canonical.");
        Kind = kind; DurationNs = durationNs; Direction = direction; DipColor = dipColor;
    }
    private static bool Timed(long duration) => duration is > 0 and <= 5_000_000_000;
    private static bool IsColor(string? value) => value is { Length: 7 } && value[0] == '#' && value.AsSpan(1).ContainsAnyExcept("0123456789abcdefABCDEF") == false;
    // Converts the existing UI's defaults; unknown modes never silently become Cut.
    public static NativeTakeTransition FromLegacy(string mode, int durationMs = 300,
        string direction = "left-to-right", string dipColor = "#000000") => mode switch
    {
        "cut" => new("cut", 0),
        "fade" => new("fade", checked((long)durationMs * 1_000_000)),
        "dip" => new("dip", checked((long)durationMs * 1_000_000), dipColor: dipColor.ToUpperInvariant()),
        "wipe" => new("wipe", checked((long)durationMs * 1_000_000), direction: direction),
        _ => throw new ArgumentException("Unknown transition mode.", nameof(mode))
    };
}

public sealed class NativeTakePlanStamp
{
    [JsonPropertyName("authorityEpoch")] public string AuthorityEpoch { get; }
    [JsonPropertyName("registryEpoch")] public string RegistryEpoch { get; }
    [JsonPropertyName("controlRevision")] public long ControlRevision { get; }
    [JsonPropertyName("registryRevision")] public long RegistryRevision { get; }
    [JsonPropertyName("eligibilityIdentity")] public string EligibilityIdentity { get; }
    public NativeTakePlanStamp(string authorityEpoch, string registryEpoch, long controlRevision, long registryRevision, string eligibilityIdentity)
    {
        AuthorityEpoch = NativeTakeValidation.Text(authorityEpoch, nameof(authorityEpoch), 256);
        RegistryEpoch = NativeTakeValidation.Text(registryEpoch, nameof(registryEpoch), 256);
        ControlRevision = NativeTakeValidation.Revision(controlRevision, nameof(controlRevision));
        RegistryRevision = NativeTakeValidation.Revision(registryRevision, nameof(registryRevision));
        EligibilityIdentity = NativeTakeValidation.Text(eligibilityIdentity, nameof(eligibilityIdentity), 4 * 1024 * 1024);
    }
}

public sealed class NativeTakePreparationBase
{
    [JsonPropertyName("epoch")] public string Epoch { get; }
    [JsonPropertyName("revision")] public long Revision { get; }
    [JsonPropertyName("generation")] public long Generation { get; }
    public NativeTakePreparationBase(string epoch, long revision, long generation)
    {
        Epoch = NativeTakeValidation.Text(epoch, nameof(epoch), 256);
        Revision = NativeTakeValidation.Revision(revision, nameof(revision));
        Generation = NativeTakeValidation.Revision(generation, nameof(generation), true);
    }
}

// A reference to native preparation, never a client-minted readiness certificate.
public sealed class NativeTakePreparationToken
{
    [JsonPropertyName("base")] public NativeTakePreparationBase Base { get; }
    [JsonPropertyName("planId")] public string PlanId { get; }
    [JsonPropertyName("planRevision")] public long PlanRevision { get; }
    [JsonPropertyName("transactionGeneration")] public long TransactionGeneration { get; }
    [JsonPropertyName("stamp")] public NativeTakePlanStamp Stamp { get; }
    public NativeTakePreparationToken(NativeTakePreparationBase @base, string planId, long planRevision, long transactionGeneration, NativeTakePlanStamp stamp)
    {
        ArgumentNullException.ThrowIfNull(@base); ArgumentNullException.ThrowIfNull(stamp);
        Base = @base; Stamp = stamp;
        PlanId = NativeTakeValidation.Text(planId, nameof(planId), 256);
        PlanRevision = NativeTakeValidation.Revision(planRevision, nameof(planRevision), true);
        TransactionGeneration = NativeTakeValidation.Revision(transactionGeneration, nameof(transactionGeneration), true);
        if (@base.Revision >= NativeTakeValidation.MaxSafeInteger || planRevision != @base.Revision + 1 ||
            stamp.AuthorityEpoch != @base.Epoch || stamp.ControlRevision != @base.Revision)
            throw new ArgumentException("Preparation must describe exactly the next revision of its input basis.");
    }
}

public sealed class NativeTakeFingerprint
{
    [JsonPropertyName("expectedRevision")] public long ExpectedRevision { get; }
    [JsonPropertyName("previewRevision")] public long PreviewRevision { get; }
    [JsonPropertyName("mediaProcessEpoch")] public string MediaProcessEpoch { get; }
    [JsonPropertyName("mediaGeneration")] public long MediaGeneration { get; }
    [JsonPropertyName("transition")] public NativeTakeTransition Transition { get; }
    [JsonPropertyName("expectedPlanStamp")] public NativeTakePlanStamp ExpectedPlanStamp => Preparation.Stamp;
    [JsonPropertyName("expectedPlanId")] public string ExpectedPlanId => Preparation.PlanId;
    [JsonPropertyName("preparation")] public NativeTakePreparationToken Preparation { get; }
    public NativeTakeFingerprint(long expectedRevision, long previewRevision, string mediaProcessEpoch, long mediaGeneration,
        NativeTakeTransition transition, NativeTakePreparationToken preparation)
    {
        ArgumentNullException.ThrowIfNull(transition); ArgumentNullException.ThrowIfNull(preparation);
        ExpectedRevision = NativeTakeValidation.Revision(expectedRevision, nameof(expectedRevision));
        PreviewRevision = NativeTakeValidation.Revision(previewRevision, nameof(previewRevision));
        MediaProcessEpoch = NativeTakeValidation.Text(mediaProcessEpoch, nameof(mediaProcessEpoch));
        MediaGeneration = NativeTakeValidation.Revision(mediaGeneration, nameof(mediaGeneration), true);
        Transition = transition; Preparation = preparation;
        if (preparation.Base.Revision != expectedRevision) throw new ArgumentException("Expected revision differs from preparation basis.");
    }
}

public sealed class NativeTakeRequest
{
    [JsonPropertyName("type")] public string Type => "take";
    [JsonPropertyName("authorityEpoch")] public string AuthorityEpoch { get; }
    [JsonPropertyName("operationId")] public string OperationId { get; }
    [JsonPropertyName("fingerprint")] public NativeTakeFingerprint Fingerprint { get; }
    public NativeTakeRequest(string authorityEpoch, string operationId, NativeTakeFingerprint fingerprint)
    {
        ArgumentNullException.ThrowIfNull(fingerprint);
        AuthorityEpoch = NativeTakeValidation.Text(authorityEpoch, nameof(authorityEpoch), 256);
        OperationId = NativeTakeValidation.Text(operationId, nameof(operationId));
        Fingerprint = fingerprint;
        if (fingerprint.Preparation.Base.Epoch != authorityEpoch) throw new ArgumentException("Authority epoch differs from preparation basis.");
    }
    public string Serialize() => JsonSerializer.Serialize(this);
}

public sealed class NativeTakeOutcome
{
    [JsonPropertyName("authorityEpoch")] public string AuthorityEpoch { get; }
    [JsonPropertyName("operationId")] public string OperationId { get; }
    [JsonPropertyName("error")] public string Error { get; }
    [JsonPropertyName("pending")] public bool Pending { get; }
    [JsonPropertyName("accepted")] public bool Accepted { get; }
    [JsonPropertyName("applied")] public bool Applied { get; }
    [JsonPropertyName("rendered")] public bool Rendered { get; }
    [JsonPropertyName("delivered")] public bool Delivered { get; }
    [JsonPropertyName("resultRevision")] public long ResultRevision { get; }
    [JsonPropertyName("failure")] public string Failure { get; }
    [JsonConstructor]
    public NativeTakeOutcome(string authorityEpoch, string operationId, string error, bool pending, bool accepted,
        bool applied, bool rendered, bool delivered, long resultRevision, string failure)
    {
        AuthorityEpoch = NativeTakeValidation.Text(authorityEpoch, nameof(authorityEpoch), 256);
        OperationId = NativeTakeValidation.Text(operationId, nameof(operationId));
        Error = NativeTakeValidation.Text(error, nameof(error), 64);
        ArgumentNullException.ThrowIfNull(failure);
        if (Encoding.UTF8.GetByteCount(failure) > 512 || (pending && (!accepted || applied || rendered || delivered)) ||
            (applied && !accepted) || (rendered && !applied) || (delivered && !rendered) || (error != "none" && (applied || rendered || delivered)))
            throw new ArgumentException("Inconsistent Take outcome evidence.");
        Pending=pending; Accepted=accepted; Applied=applied; Rendered=rendered; Delivered=delivered; Failure=failure;
        ResultRevision=NativeTakeValidation.Revision(resultRevision,nameof(resultRevision));
    }
}
