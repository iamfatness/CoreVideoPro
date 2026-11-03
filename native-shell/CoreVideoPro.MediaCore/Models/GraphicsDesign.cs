using System.Text.Json;
using System.Text.RegularExpressions;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>A graphic the operator built in the Graphics designer, stored in the show's graphics
/// document (schema 3). <see cref="Definition"/> is the native template manifest; the native
/// registry still owns the 16-definition and 256 KiB limits.</summary>
public sealed record GraphicsDesign(string DesignId, string Name, string Family, string TemplateId, int TemplateVersion, JsonElement Definition, IReadOnlyList<GraphicsDesignAsset> Assets, DateTimeOffset UpdatedAt);

/// <summary>An image a design references: content hash plus a path relative to the show's asset folder.</summary>
public sealed record GraphicsDesignAsset(string AssetId, string Sha256, string RelativePath);

/// <summary>Design template ids: <c>design-&lt;slug&gt;-&lt;6 lowercase hex&gt;</c>, at most 64 characters.
/// The fixed prefix keeps them disjoint from every built-in template id.</summary>
public static class GraphicsDesignIds
{
    public const int MaxLength = 64, MaxDesigns = 32, MaxNameLength = 80;
    private const string Prefix = "design-";
    private const int SlugBudget = MaxLength - 7 - 7; // "design-" + slug + "-" + 6 hex
    private static readonly Regex Shape = new(@"^design-[a-z0-9]+(-[a-z0-9]+)*-[0-9a-f]{6}\z", RegexOptions.CultureInvariant);

    /// <summary>The native id rule (validTemplateId): [a-z0-9-], leading letter, at most 64 characters.</summary>
    public static bool IsNativeId(string? id) => id is { Length: > 0 and <= MaxLength } && id[0] is >= 'a' and <= 'z' &&
        id.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');

    public static bool IsDesignTemplateId(string templateId) =>
        templateId is { Length: <= MaxLength } && Shape.IsMatch(templateId);

    public static string NewTemplateId(string name, IEnumerable<string> existingTemplateIds, Func<string> hex6)
    {
        var slug = Slug(name ?? "");
        var existing = new HashSet<string>(existingTemplateIds, StringComparer.Ordinal);
        for (var attempt = 0; attempt < 1000; attempt++) {
            var id = Prefix + slug + "-" + hex6();
            if (IsDesignTemplateId(id) && !existing.Contains(id)) return id;
        }
        throw new InvalidOperationException("Could not generate a unique design template id.");
    }

    private static string Slug(string name)
    {
        var builder = new System.Text.StringBuilder();
        foreach (var c in name.ToLowerInvariant()) {
            if (c is >= 'a' and <= 'z' or >= '0' and <= '9') builder.Append(c);
            else if (builder.Length > 0 && builder[^1] != '-') builder.Append('-');
        }
        var slug = builder.ToString();
        if (slug.Length > SlugBudget) slug = slug[..SlugBudget];
        slug = slug.Trim('-');
        return slug.Length == 0 ? "untitled" : slug;
    }
}
