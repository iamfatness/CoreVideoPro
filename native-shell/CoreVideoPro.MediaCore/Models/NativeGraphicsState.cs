using CoreVideoPro.MediaCore.Contracts;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>Native accepted graphics revisions; no shell-derived on-air state.</summary>
public sealed record NativeGraphicsState
{
    public int SchemaVersion { get; init; }
    public string CoreEpoch { get; init; } = "";
    public IReadOnlyList<NativeGraphicsInstance> Instances { get; init; } = [];
    public NativeGraphicsOperationResult? LastResult { get; init; }
}

public sealed record NativeGraphicsInstance
{
    public GraphicsRevisionIdentity? Identity { get; init; }
    public GraphicsPreparationFact? PreparationObservation { get; init; }
    public GraphicsCompositionFact? FirstCompositionObservation { get; init; }
    public string InstanceId { get; init; } = "";
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public string Scope { get; init; } = "";
    public string SceneId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public long Revision { get; init; }
    public long ProgramRevision { get; init; }
    public string Preparation { get; init; } = "unprepared";
    public string Visibility { get; init; } = "hidden";
    public bool Cued { get; init; }
    public bool ShowPending { get; init; }
    /// <summary>First native draw submission. Does not establish receiver delivery.</summary>
    public long FirstDrawSubmittedFrame { get; init; }
    public string Warning { get; init; } = "";
    public NativeGraphicsFields Fields { get; init; } = new();
    public LowerThirdAppearance Style { get; init; } = new();
    public NativeGraphicsAnimations Animations { get; init; } = new();
    public string ProgramSourceId { get; init; } = "";
    public NativeGraphicsFields? ProgramFields { get; init; }
    public LowerThirdAppearance? ProgramStyle { get; init; }
    public NativeGraphicsAnimations? ProgramAnimations { get; init; }
}

public sealed record NativeGraphicsAnimations
{
    public NativeGraphicsTransition In { get; init; } = new();
    public NativeGraphicsTransition Out { get; init; } = new();
}

public sealed record NativeGraphicsTransition
{
    public string Type { get; init; } = "none";
    public string Direction { get; init; } = "left";
    public string Easing { get; init; } = "linear";
    public double DurationMs { get; init; }
}

public sealed record NativeGraphicsFields
{
    public string Name { get; init; } = "";
    public string Title { get; init; } = "";
}

public sealed record NativeGraphicsOperationResult
{
    public string RequestId { get; init; } = "";
    public bool Accepted { get; init; }
    public string Reason { get; init; } = "";
}
