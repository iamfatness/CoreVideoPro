namespace CoreVideoPro.MediaCore.Models;
public sealed record LowerThirdPreview(LowerThirdAppearance? Appearance,string Name="Example guest",string Title="Producer",
    string BrandColor="#44C1A1",string BackgroundColor="#0C1118",string ImageUri="",string Position="lower-left",string FontFamily="Inter",
    NativeGraphicsPreview? Graphics=null);

public sealed record NativeGraphicsDraft(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, LowerThirdAppearance Style, NativeGraphicsAnimations Animations)
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
    public NativeGraphicsPreview ForPreview() => new(TemplateId, TemplateVersion, Fields, StyleWire(), Animations);
}
public sealed record NativeGraphicsPreview(string TemplateId, int TemplateVersion,
    System.Text.Json.JsonElement Fields, System.Text.Json.JsonElement Style, NativeGraphicsAnimations Animations);
