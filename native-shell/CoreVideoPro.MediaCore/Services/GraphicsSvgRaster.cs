using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;
using SkiaSharp;
using Svg.Skia;

namespace CoreVideoPro.MediaCore.Services;

internal static class GraphicsSvgRaster
{
    private const int MaxBytes = 1024 * 1024;
    private static readonly XNamespace Svg = "http://www.w3.org/2000/svg";
    private static readonly HashSet<string> Elements = new(StringComparer.Ordinal)
    {
        "svg", "g", "defs", "title", "desc", "path", "rect", "circle", "ellipse", "line", "polyline", "polygon",
        "linearGradient", "radialGradient", "stop", "clipPath"
    };
    private static readonly Regex LocalPaint = new(@"url\(\s*#[a-zA-Z_][\w.-]*\s*\)", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    internal static byte[] ToPng(byte[] source, int layerWidth, int layerHeight)
    {
        if (source.Length > MaxBytes) throw new InvalidDataException("SVG logos must be no larger than 1 MiB.");
        using var input = new MemoryStream(source, writable: false);
        using var reader = XmlReader.Create(input, new XmlReaderSettings
        {
            DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = MaxBytes
        });
        XDocument document;
        try { document = XDocument.Load(reader); }
        catch (XmlException ex) { throw new InvalidDataException("The SVG is not valid static XML.", ex); }
        var root = document.Root;
        if (root?.Name != Svg + "svg" || root.DescendantNodes().OfType<XProcessingInstruction>().Any()) throw new InvalidDataException("Choose a static SVG logo.");
        var count = 0;
        foreach (var element in root.DescendantsAndSelf())
        {
            if (++count > 4096 || element.Ancestors().Take(33).Count() > 32 || element.Name.Namespace != Svg || !Elements.Contains(element.Name.LocalName))
                throw new InvalidDataException("The SVG contains unsupported or overly complex content.");
            foreach (var attribute in element.Attributes().Where(attribute => !attribute.IsNamespaceDeclaration))
            {
                var name = attribute.Name.LocalName;
                var value = attribute.Value;
                if (attribute.Name.Namespace != XNamespace.None || name.StartsWith("on", StringComparison.OrdinalIgnoreCase) ||
                    name is "href" or "src" || value.IndexOfAny(['@', '\\']) >= 0 ||
                    LocalPaint.Replace(value, "").Contains("url", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The SVG may only reference artwork defined inside the file.");
            }
        }

        using var staticSvg = new MemoryStream();
        root.Save(staticSvg, SaveOptions.DisableFormatting);
        staticSvg.Position = 0;
        using var svg = new SKSvg();
        var picture = svg.Load(staticSvg) ?? throw new InvalidDataException("The SVG could not be drawn.");
        var bounds = picture.CullRect;
        if (!float.IsFinite(bounds.Width) || !float.IsFinite(bounds.Height) || bounds.Width <= 0 || bounds.Height <= 0)
            throw new InvalidDataException("The SVG needs a positive view box.");
        var width = Math.Min(4096, layerWidth * 2);
        var height = Math.Min(4096, layerHeight * 2);
        using var surface = SKSurface.Create(new SKImageInfo(width, height, SKColorType.Rgba8888, SKAlphaType.Premul))
            ?? throw new InvalidDataException("The SVG raster could not be allocated.");
        var canvas = surface.Canvas;
        canvas.Clear(SKColors.Transparent);
        var scale = Math.Min(width / bounds.Width, height / bounds.Height);
        canvas.Translate((width - bounds.Width * scale) / 2, (height - bounds.Height * scale) / 2);
        canvas.Scale(scale);
        canvas.Translate(-bounds.Left, -bounds.Top);
        canvas.DrawPicture(picture);
        using var image = surface.Snapshot();
        using var png = image.Encode(SKEncodedImageFormat.Png, 100) ?? throw new InvalidDataException("The SVG could not be encoded.");
        return png.ToArray();
    }
}
