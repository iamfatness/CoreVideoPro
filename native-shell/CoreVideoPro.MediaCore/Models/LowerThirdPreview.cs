namespace CoreVideoPro.MediaCore.Models;
public sealed record LowerThirdPreview(
    [property: System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.Never)]
    LowerThirdAppearance? Appearance,string Name="Example guest",string Title="Producer",
    string BrandColor="#44C1A1",string BackgroundColor="#0C1118",string ImageUri="",string Position="lower-left",string FontFamily="Inter",
    NativeGraphicsPreview? Graphics=null);

public sealed record NativeGraphicsDraft(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, LowerThirdAppearance Style, NativeGraphicsAnimations Animations, NativeGraphicsPlacement? Placement = null, NativeGraphicsScratch? Scratch = null, NativeGraphicsImageReference? Image = null)
{
    // Live generations are process-local and must be explicitly rebound on restore.
    [System.Text.Json.Serialization.JsonIgnore]
    public CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity? Binding { get; init; }
    [System.Text.Json.Serialization.JsonIgnore]
    public bool Declarative => TemplateId is not ("compact-solid" or "minimal-accent" or "broadcast");
    public System.Text.Json.JsonElement StyleWire()
    {
        var options = new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web);
        var style = System.Text.Json.JsonSerializer.SerializeToElement(Style, options);
        if (!Declarative) return System.Text.Json.JsonSerializer.SerializeToElement(style.EnumerateObject().Where(p=>p.Name!="isValid").ToDictionary(p=>p.Name,p=>p.Value.Clone(),StringComparer.Ordinal),options);
        var consumed = new HashSet<string>(StringComparer.Ordinal) { "version", "anchor", "fontFamily", "nameColor", "titleColor", "backgroundColor", "accentColor", "backgroundOpacity", "safeOffsetX", "safeOffsetY" };
        return System.Text.Json.JsonSerializer.SerializeToElement(style.EnumerateObject().Where(p => consumed.Contains(p.Name))
            .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal), options);
    }
    public NativeGraphicsPreview ForPreview() => new(TemplateId, TemplateVersion, Fields, StyleWire(), Animations, Placement, Scratch, Image);
}
public sealed record NativeGraphicsPreview(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, System.Text.Json.JsonElement Style, NativeGraphicsAnimations Animations, NativeGraphicsPlacement? Placement = null, NativeGraphicsScratch? Scratch = null, NativeGraphicsImageReference? Image = null);

public sealed record NativeGraphicsPlacement
{
    public static readonly string[] Anchors = ["upper-left","upper-center","upper-right","center-left","center","center-right","lower-left","lower-center","lower-right"];
    public string Anchor { get; init; } = "lower-left";
    public double X { get; init; } = 96;
    public double Y { get; init; } = -54;
    public double Scale { get; init; } = 1;
    [System.Text.Json.Serialization.JsonIgnore]
    public bool IsValid => Anchors.Contains(Anchor,StringComparer.Ordinal) && double.IsFinite(X) && Math.Abs(X)<=3840 && double.IsFinite(Y) && Math.Abs(Y)<=2160 && double.IsFinite(Scale) && Scale is >=.1 and <=4;
}

public sealed record NativeGraphicsScratch(string Manifest,string Hash)
{
    public static NativeGraphicsScratch From(System.Text.Json.JsonElement definition)
    {
        var manifest=definition.GetRawText(); var bytes=System.Text.Encoding.UTF8.GetBytes(manifest);
        if(bytes.Length>256*1024) throw new System.IO.InvalidDataException("Template exceeds the 256 KiB limit.");
        return new(manifest,Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes)).ToLowerInvariant());
    }
}

public sealed record NativeGraphicsImageReference(string TemplateId,int TemplateVersion,string AssetId);
