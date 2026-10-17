namespace CoreVideoPro.MediaCore.Models;
public sealed record LowerThirdPreview(LowerThirdAppearance? Appearance,string Name="Example guest",string Title="Producer",
    string BrandColor="#44C1A1",string BackgroundColor="#0C1118",string ImageUri="",string Position="lower-left",string FontFamily="Inter",
    NativeGraphicsPreview? Graphics=null);

public sealed record NativeGraphicsDraft(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, LowerThirdAppearance Style, NativeGraphicsAnimations Animations, NativeGraphicsPlacement? Placement = null)
{
    [System.Text.Json.Serialization.JsonIgnore]
    public bool Declarative => TemplateId is not ("compact-solid" or "minimal-accent" or "broadcast");
    public System.Text.Json.JsonElement StyleWire()
    {
        var options = new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web);
        var style = System.Text.Json.JsonSerializer.SerializeToElement(Style, options);
        if (!Declarative) return style;
        var consumed = new HashSet<string>(StringComparer.Ordinal) { "version", "anchor", "fontFamily", "nameColor", "titleColor", "backgroundColor", "accentColor", "backgroundOpacity", "safeOffsetX", "safeOffsetY" };
        return System.Text.Json.JsonSerializer.SerializeToElement(style.EnumerateObject().Where(p => consumed.Contains(p.Name))
            .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal), options);
    }
    public NativeGraphicsPreview ForPreview() => new(TemplateId, TemplateVersion, Fields, StyleWire(), Animations, Placement);
}
public sealed record NativeGraphicsPreview(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, System.Text.Json.JsonElement Style, NativeGraphicsAnimations Animations, NativeGraphicsPlacement? Placement = null);

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
