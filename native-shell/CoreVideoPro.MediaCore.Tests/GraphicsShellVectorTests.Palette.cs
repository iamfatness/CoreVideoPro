using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Graphics styles slice 1: the show palette rules (GraphicsPalette, GraphicsPaletteMigration)
/// pinned for the Mac model (mac-shell GraphicsPalette.swift, ShellTestsGraphicsPalette.swift).
/// Regenerate with COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1.</summary>
public sealed partial class GraphicsShellVectorTests
{
    private static JsonNode PaletteNode(GraphicsPalette palette) => JsonSerializer.SerializeToNode(palette, Web)!;
    private static JsonNode? MapNode(IReadOnlyDictionary<string, string>? map) =>
        map is null ? null : JsonSerializer.SerializeToNode(new SortedDictionary<string, string>(map.ToDictionary(p => p.Key, p => p.Value), StringComparer.Ordinal), Web);

    [Fact]
    public void PaletteVectors()
    {
        var rows = new JsonArray();
        var light = GraphicsPalette.Default.With(GraphicsPalette.Primary, "#F5F5F5FF").With(GraphicsPalette.OnSecondary, "#123456FF").With(GraphicsPalette.TeamA, "#C8102EFF");
        var dark = GraphicsPaletteMigration.FromBrandKit("#13315C", "#E63946", "#0B1F3A", stock: false);
        var cases = new (string Id, GraphicsPalette Palette, Dictionary<string, string>? Overrides)[] {
            ("resolve-default", GraphicsPalette.Default, null),
            ("resolve-light-plate-pinned-team", light, null),
            ("resolve-row-override-wins", light, new() { [GraphicsPalette.Primary] = "#000000FF", [GraphicsPalette.TeamB] = "#003087FF", [GraphicsPalette.OnAccent] = "#ABCDEF80" }),
            ("resolve-brand-kit", dark, null),
            ("resolve-lowercase-override", GraphicsPalette.Default, new() { [GraphicsPalette.Accent] = "#e63946ff" }),
        };
        foreach (var (id, palette, overrides) in cases)
            rows.Add(Row(id, "resolve", new JsonObject { ["palette"] = PaletteNode(palette), ["rowOverrides"] = overrides is null ? null : MapNode(overrides) },
                MapNode(palette.Resolve(overrides))!));

        var validity = new (string Id, GraphicsPalette Palette)[] {
            ("valid-default", GraphicsPalette.Default), ("valid-with-team", light),
            ("invalid-missing-required", GraphicsPalette.Default with { Colors = new Dictionary<string, string> { [GraphicsPalette.Primary] = "#FFFFFFFF" } }),
            ("invalid-auto-plate", GraphicsPalette.Default.With(GraphicsPalette.Primary, GraphicsPalette.Auto)),
            ("invalid-six-digit", GraphicsPalette.Default.With(GraphicsPalette.Accent, "#FFFFFF")),
            ("invalid-unknown-token", GraphicsPalette.Default.With("brand.logo", "#FFFFFFFF")),
            ("invalid-version", GraphicsPalette.Default with { Version = 2 }),
        };
        foreach (var (id, palette) in validity)
            rows.Add(Row(id, "validate", new JsonObject { ["palette"] = PaletteNode(palette) }, new JsonObject { ["valid"] = palette.IsValid }));

        foreach (var text in new[] { "#abc", "e63946", "#0A0F18B3", "", "#12345", "#GGGGGG", "navy", " #FfFfFf " })
            rows.Add(Row("normalize-" + (text.Trim().Length == 0 ? "empty" : text.Trim().TrimStart('#').ToLowerInvariant()), "normalize", new JsonObject { ["text"] = text },
                new JsonObject { ["value"] = GraphicsPalette.TryNormalize(text, out var value) ? value : null }));

        foreach (var (a, b) in new[] { ("#000000FF", "#FFFFFFFF"), ("#172433E6", "#FFFFFFFF"), ("#44C1A1FF", "#111111FF") })
            rows.Add(Row($"contrast-{a[1..7].ToLowerInvariant()}-{b[1..7].ToLowerInvariant()}", "contrast", new JsonObject { ["a"] = a, ["b"] = b },
                new JsonObject { ["ratio"] = Math.Round(GraphicsPalette.Contrast(a, b), 6) }));

        foreach (var (id, brand, accent, background, stock) in new[] { ("brand-kit-stock", "#44c1a1", "#f0a85c", "#0c1118", true), ("brand-kit-edited", "#13315C", "#E63946", "#0B1F3A", false) })
            rows.Add(Row(id, "fromBrandKit", new JsonObject { ["brandColor"] = brand, ["accentColor"] = accent, ["backgroundColor"] = background, ["stock"] = stock },
                PaletteNode(GraphicsPaletteMigration.FromBrandKit(brand, accent, background, stock))));

        var stockLook = new LowerThirdAppearance { NameColor = "#FFFFFF", TitleColor = "#E1E9F2", BackgroundColor = "#172433", AccentColor = "#44C1A1" };
        var migrations = new (string Id, string TemplateId, LowerThirdAppearance Style, GraphicsPalette Palette)[] {
            ("migrate-stock-row-follows", "centered-title", stockLook, GraphicsPalette.Default),
            ("migrate-custom-row-keeps-colours", "centered-title", stockLook with { AccentColor = "#E63946", BackgroundOpacity = .5 }, GraphicsPalette.Default),
            ("migrate-against-brand-kit", "logo-accent", stockLook, dark),
            ("migrate-compatibility-untouched", "compact-solid", stockLook with { AccentColor = "#E63946" }, GraphicsPalette.Default),
        };
        foreach (var (id, templateId, style, palette) in migrations) {
            var row = new GraphicsPlaylistRow { Id = "row", Name = "Row", TemplateId = templateId, TemplateVersion = 1, Style = style };
            rows.Add(Row(id, "migrateRow", new JsonObject { ["templateId"] = templateId, ["style"] = JsonSerializer.SerializeToNode(style, Web), ["palette"] = PaletteNode(palette) },
                new JsonObject { ["brandOverrides"] = MapNode(GraphicsPaletteMigration.MigrateRow(row, palette).BrandOverrides) }));
        }
        Check("palette", "GraphicsPalette.Resolve / IsValid / TryNormalize / Contrast and GraphicsPaletteMigration (graphics styles slice 1).", rows);
    }
}
