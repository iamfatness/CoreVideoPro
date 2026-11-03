using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using SkiaSharp;

namespace CoreVideoPro.MediaCore.Services;

public static partial class GraphicsTemplateEditor
{
    public static async Task<(NativeGraphicsScratch Scratch, string Id)> ImportImageAsync(
        NativeGraphicsScratch scratch, string path, string assetRoot, CancellationToken cancellation = default)
    {
        var root = Read(scratch);
        var bounds = root["bounds"]!.AsObject();
        var width = Math.Clamp((int)(bounds["width"]?.GetValue<double>() ?? 192), 1, 192);
        var height = Math.Clamp((int)(bounds["height"]?.GetValue<double>() ?? 192), 1, 192);
        var places = Places(root);
        if (places.Count >= MaxLayers) throw new InvalidOperationException("A template supports at most 64 layers.");
        var staged = await GraphicsImageImport.StageAsync(path, assetRoot, cancellation, layerWidth: width, layerHeight: height);
        var declaration = JsonNode.Parse(staged.Package.Manifest)!["assets"]![0]!.AsObject();
        var file = declaration["file"]!.GetValue<string>();
        var hash = declaration["sha256"]!.GetValue<string>();
        var destination = VerifiedPath(staged.Package.PackageRoot, file);
        using (var image = SKCodec.Create(destination))
        {
            if (image is null || image.Info.Width is < 1 or > 4096 || image.Info.Height is < 1 or > 4096 ||
                image.EncodedFormat != (file.EndsWith(".png", StringComparison.Ordinal) ? SKEncodedImageFormat.Png : SKEncodedImageFormat.Jpeg))
                throw new InvalidDataException("Choose a valid PNG, JPEG or SVG image up to 4096 pixels per side.");
        }

        var assets = root["assets"] as JsonArray;
        if (assets is null) root["assets"] = assets = new JsonArray();
        var currentRoot = scratch.PackageRoot;
        if (!string.Equals(currentRoot, staged.Package.PackageRoot, StringComparison.OrdinalIgnoreCase))
        {
            foreach (var asset in assets.OfType<JsonObject>())
            {
                var oldFile = asset["file"]!.GetValue<string>();
                if (string.IsNullOrEmpty(currentRoot))
                {
                    if (asset["optional"]?.GetValue<bool>() == true) continue;
                    throw new InvalidDataException("The design's previous asset location is unavailable.");
                }
                var oldPath = VerifiedPath(currentRoot, oldFile);
                if (!File.Exists(oldPath) && asset["optional"]?.GetValue<bool>() == true) continue;
                var migrated = await GraphicsImageImport.StageAsync(oldPath, staged.Package.PackageRoot, cancellation);
                var copied = JsonNode.Parse(migrated.Package.Manifest)!["assets"]![0]!;
                if (copied["sha256"]!.GetValue<string>() != asset["sha256"]!.GetValue<string>())
                    throw new InvalidDataException("A previous design asset changed on disk.");
                asset["file"] = copied["file"]!.GetValue<string>();
            }
        }

        var assetId = assets.OfType<JsonObject>().FirstOrDefault(asset => asset["sha256"]?.GetValue<string>() == hash)?["id"]?.GetValue<string>();
        if (assetId is null)
        {
            if (assets.Count >= 32) throw new InvalidOperationException("A design supports at most 32 image assets.");
            assetId = "image-" + hash[..32];
            assets.Add(new JsonObject { ["id"] = assetId, ["file"] = file, ["sha256"] = hash });
        }
        var id = NextId(places.Keys, "image");
        root["layers"]!.AsArray().Add(new JsonObject { ["id"] = id, [LayerTypeKey] = "image", ["asset"] = assetId,
            ["x"] = 0, ["y"] = 0, ["width"] = width, ["height"] = height, ["fit"] = "contain",
            ["metadata"] = new JsonObject { ["label"] = "Imported image" } });
        return (Commit(scratch, root) with { PackageRoot = staged.Package.PackageRoot }, id);
    }

    private static string VerifiedPath(string root, string relative)
    {
        var full = Path.GetFullPath(Path.Combine(root, relative));
        var underRoot = Path.GetRelativePath(root, full);
        if (Path.IsPathRooted(underRoot) || underRoot == ".." || underRoot.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal))
            throw new InvalidDataException("The asset is outside the design package.");
        GraphicsPackageReader.NoReparse(full);
        return full;
    }
}
