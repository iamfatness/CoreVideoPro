using System.Text.Json.Nodes;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>The four blank starting points for a new design. The manifests live in
/// <c>contracts/graphics-seeds</c> so the native tests load the exact same files.</summary>
public static class GraphicsDesignSeeds
{
    public static IReadOnlyList<string> Kinds { get; } = ["lower-third", "title", "bug", "full-screen"];

    /// <summary>A fresh copy of the seed for <paramref name="kind"/>, stamped with the design's identity.</summary>
    public static JsonObject Manifest(string kind, string templateId, int templateVersion, string name)
    {
        if (!Kinds.Contains(kind)) throw new ArgumentException($"Unknown graphics design seed '{kind}'.", nameof(kind));
        using var stream = typeof(GraphicsDesignSeeds).Assembly.GetManifestResourceStream($"CoreVideoPro.Graphics.Seed.{kind}.json")
            ?? throw new InvalidOperationException($"Graphics design seed '{kind}' is not embedded.");
        var manifest = JsonNode.Parse(stream)!.AsObject();
        manifest["templateId"] = templateId;
        manifest["templateVersion"] = templateVersion;
        manifest["name"] = name;
        return manifest;
    }
}
