using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsShowStarter(string Id, string Name, string Description, string LowerThird, string Accent, string Heading, string Secondary);
public sealed record GraphicsStarterGraphic(string Name, string Purpose, NativeGraphicsTemplate Template, NativeGraphicsDraft Draft);

/// <summary>Coordinated examples built from shipped native templates, never shell pixels.</summary>
public static class GraphicsShowStarters
{
    public static IReadOnlyList<GraphicsStarterGraphic> WithBranding(IReadOnlyList<GraphicsStarterGraphic> graphics, string title, string color)
    {
        if (string.IsNullOrWhiteSpace(title) || System.Text.Encoding.UTF8.GetByteCount(title)>256 || color.Length!=7 || color[0]!='#' || !color.AsSpan(1).ToArray().All(Uri.IsHexDigit))
            throw new InvalidOperationException("Enter a show title of up to 256 UTF-8 bytes and a color such as #44C1A1.");
        return graphics.Select(g=> {
            var fields=g.Draft.Fields.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
            if(fields.ContainsKey("heading"))fields["heading"]=JsonSerializer.SerializeToElement(title.Trim());
            var style=g.Draft.Style with {AccentColor=color};
            if(g.Draft.Declarative)style=style with {BackgroundColor=color};
            return g with {Draft=g.Draft with {Fields=JsonSerializer.SerializeToElement(fields),Style=style}};
        }).ToArray();
    }

    public static IReadOnlyList<GraphicsStarterGraphic> WithLogo(IReadOnlyList<GraphicsStarterGraphic> graphics, NativeGraphicsTemplate template, NativeGraphicsImageReference image)
    {
        if(template.TemplateId!=image.TemplateId || template.TemplateVersion!=image.TemplateVersion || template.Family!="bug")
            throw new InvalidOperationException("The verified logo template is unavailable. Your samples were kept.");
        return graphics.Select(g=>g.Template.Family=="bug"
            ? g with {Template=template,Draft=new(template.TemplateId,template.TemplateVersion,JsonSerializer.SerializeToElement(new {}),g.Draft.Style,g.Draft.Animations,g.Draft.Placement)}
            : !g.Draft.Declarative ? g with {Draft=g.Draft with {Image=image,Style=g.Draft.Style with {ShowLogo=true}}} : g).ToArray();
    }

    public static IReadOnlyList<GraphicsShowStarter> All { get; } = [
        new("interview", "Interview", "Introduce a host and guest with a clean, personal look.", "minimal-accent", "#44C1A1", "In conversation", "Stories worth sharing"),
        new("panel", "Panel discussion", "A bold look for multiple voices and live discussion.", "broadcast", "#609BFF", "The roundtable", "Ideas from every angle"),
        new("webinar", "Webinar", "A coordinated look for presentations and teaching.", "compact-solid", "#B99AFF", "Learn something new", "A live masterclass"),
        new("branded", "Simple branded show", "An easy starting point for a regular live show.", "compact-solid", "#FFB76B", "Your live show", "Welcome — glad you are here")
    ];

    public static IReadOnlyList<GraphicsStarterGraphic> Prepare(GraphicsShowStarter starter, NativeGraphicsState state)
    {
        if (!All.Contains(starter)) throw new InvalidOperationException("Choose an available show template.");
        var requests = new[] {
            (starter.LowerThird, "Guest introduction", "Names and titles for your guests"),
            ("centered-title", "Welcome title", "Introduce your show or the next topic"),
            ("corner-logo", "Show logo", "Sample logo — replace it with your own"),
            ("clock-panel", "Live clock", "A clock you can configure after adding the show")
        };
        return requests.Select(request => {
            var template = state.Templates.Where(t => t.TemplateId == request.Item1).OrderByDescending(t => t.TemplateVersion).FirstOrDefault()
                ?? throw new InvalidOperationException($"{starter.Name} needs the built-in {request.Item1} template. Connect to the updated media core and try again.");
            var fields = template.Runtime == "declarative"
                ? template.Fields.ToDictionary(f => f.Id, f => f.Default?.Clone() ?? JsonSerializer.SerializeToElement(""), StringComparer.Ordinal)
                : new Dictionary<string, JsonElement>(StringComparer.Ordinal) { ["name"] = JsonSerializer.SerializeToElement("Alex Rivera"), ["title"] = JsonSerializer.SerializeToElement("Host · " + starter.Name) };
            void Sample(string key, string value) { if (fields.ContainsKey(key)) fields[key] = JsonSerializer.SerializeToElement(value); }
            Sample("name", "Alex Rivera"); Sample("title", "Host · " + starter.Name);
            Sample("heading", starter.Heading); Sample("subtitle", starter.Secondary);
            Sample("label", "LIVE"); Sample("time", "12:34:56");
            var style = LowerThirdAppearance.FromPreset(template.TemplateId) with { AccentColor = starter.Accent, BackgroundColor = "#172433", NameColor = "#FFFFFF", TitleColor = "#E1E9F2" };
            var motion = new NativeGraphicsAnimations {
                In = new() { Type = "fade", DurationMs = 250, Easing = "ease-out" },
                Out = new() { Type = "fade", DurationMs = 200, Easing = "ease-in" }
            };
            return new GraphicsStarterGraphic(request.Item2, request.Item3, template,
                new(template.TemplateId, template.TemplateVersion, JsonSerializer.SerializeToElement(fields), style, motion, template.DefaultPlacement));
        }).ToArray();
    }
}
