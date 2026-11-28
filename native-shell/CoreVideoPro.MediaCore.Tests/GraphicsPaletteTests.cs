using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Graphics styles slice 1: the show palette model, migration from the brand kit,
/// persistence, the gateway shape and live re-theme (Cue now, Program on Update on air).</summary>
public sealed class GraphicsPaletteTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-palette-" + Guid.NewGuid().ToString("N"));
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    // ---- model ----------------------------------------------------------------------------
    [Fact] public void StockDefaultResolvesToTheLookBuiltInGraphicsHadBeforeThePalette()
    {
        var resolved = GraphicsPalette.Default.Resolve();
        Assert.True(GraphicsPalette.Default.IsValid);
        Assert.Equal("#172433E6", resolved[GraphicsPalette.Primary]);
        Assert.Equal("#FFFFFFFF", resolved[GraphicsPalette.OnPrimary]);
        Assert.Equal("#E1E9F2FF", resolved[GraphicsPalette.OnPrimaryMuted]);
        Assert.Equal("#44C1A1FF", resolved[GraphicsPalette.Accent]);
        Assert.Equal("#111111FF", resolved[GraphicsPalette.OnAccent]);
        Assert.False(resolved.ContainsKey(GraphicsPalette.TeamA));
        // Every value the core receives is strict #RRGGBBAA.
        Assert.All(resolved.Values, v => Assert.True(GraphicsPalette.IsColor(v), v));
    }

    [Fact] public void RowOverrideBeatsShowPaletteAndPinnedOnColoursBeatAuto()
    {
        var palette = GraphicsPalette.Default.With(GraphicsPalette.Primary, "#F5F5F5FF").With(GraphicsPalette.OnSecondary, "#123456FF");
        var resolved = palette.Resolve();
        Assert.Equal("#111111FF", resolved[GraphicsPalette.OnPrimary]); // auto on a light plate
        Assert.Equal(GraphicsPalette.MutedOnLight, resolved[GraphicsPalette.OnPrimaryMuted]);
        Assert.Equal("#123456FF", resolved[GraphicsPalette.OnSecondary]); // pinned
        var row = palette.Resolve(new Dictionary<string, string> { [GraphicsPalette.Primary] = "#000000FF", [GraphicsPalette.TeamA] = "#C8102EFF" });
        Assert.Equal("#000000FF", row[GraphicsPalette.Primary]);
        Assert.Equal("#FFFFFFFF", row[GraphicsPalette.OnPrimary]); // auto follows the overridden plate
        Assert.Equal("#C8102EFF", row[GraphicsPalette.TeamA]);
    }

    [Theory]
    [InlineData("#abc", "#AABBCCFF")] [InlineData("e63946", "#E63946FF")] [InlineData("#0A0F18B3", "#0A0F18B3")]
    public void HexInputNormalizesToRrggbbaa(string text, string expected)
    {
        Assert.True(GraphicsPalette.TryNormalize(text, out var value)); Assert.Equal(expected, value);
    }
    [Theory] [InlineData("")] [InlineData("#12345")] [InlineData("#GGGGGG")] [InlineData("navy")]
    public void InvalidHexIsRefused(string text) => Assert.False(GraphicsPalette.TryNormalize(text, out _));

    [Fact] public void ContrastMatchesWcag()
    {
        Assert.Equal(21, GraphicsPalette.Contrast("#000000FF", "#FFFFFFFF"), 3);
        Assert.Equal(1, GraphicsPalette.Contrast("#777777FF", "#777777FF"), 3);
    }

    [Fact] public void InvalidPalettesAreRejected()
    {
        Assert.False((GraphicsPalette.Default with { Colors = new Dictionary<string, string> { [GraphicsPalette.Primary] = "#FFFFFFFF" } }).IsValid);
        Assert.False(GraphicsPalette.Default.With(GraphicsPalette.Primary, GraphicsPalette.Auto).IsValid); // only on-colours may be auto
        Assert.False(GraphicsPalette.Default.With(GraphicsPalette.Accent, "#FFFFFF").IsValid);
        Assert.False(GraphicsPalette.Default.With("brand.logo", "#FFFFFFFF").IsValid);
        Assert.True(GraphicsPalette.Default.With(GraphicsPalette.TeamB, "#003087FF").With(GraphicsPalette.TeamB, "").IsValid);
    }

    // ---- migration ------------------------------------------------------------------------
    [Fact] public void StockBrandKitSeedsTheStockDefaultAndAnEditedKitBecomesThePalette()
    {
        Assert.Same(GraphicsPalette.Default, GraphicsPaletteMigration.FromBrandKit("#44c1a1", "#f0a85c", "#0c1118", stock: true));
        var edited = GraphicsPaletteMigration.FromBrandKit("#13315C", "#E63946", "#0B1F3A", stock: false);
        Assert.True(edited.IsValid);
        Assert.Equal("#0B1F3AE6", edited.Colors[GraphicsPalette.Primary]);
        Assert.Equal("#13315CFF", edited.Colors[GraphicsPalette.Secondary]);
        Assert.Equal("#E63946FF", edited.Colors[GraphicsPalette.Accent]);
        Assert.Equal("#0B1F3AB3", edited.Colors[GraphicsPalette.Surface]);
        Assert.Equal(GraphicsPalette.Auto, edited.Colors[GraphicsPalette.OnPrimary]);
    }

    [Fact] public void MigrationKeepsEveryExistingRowLookingExactlyTheSame()
    {
        var stockLook = new LowerThirdAppearance { NameColor = "#FFFFFF", TitleColor = "#E1E9F2", BackgroundColor = "#172433", AccentColor = "#44C1A1" };
        var follows = new GraphicsPlaylistRow { Id = "title", Name = "Title", TemplateId = "centered-title", TemplateVersion = 1, Style = stockLook };
        Assert.Null(GraphicsPaletteMigration.MigrateRow(follows, GraphicsPalette.Default).BrandOverrides);
        var custom = follows with { Style = stockLook with { AccentColor = "#E63946", BackgroundOpacity = .5 } };
        var migrated = GraphicsPaletteMigration.MigrateRow(custom, GraphicsPalette.Default);
        Assert.Equal(new Dictionary<string, string> { [GraphicsPalette.Primary] = "#17243380", [GraphicsPalette.Accent] = "#E63946FF" }, migrated.BrandOverrides);
        Assert.True(migrated.IsValid);
        // The migrated row resolves to exactly the colours its appearance drew.
        var resolved = GraphicsPalette.Default.Resolve(migrated.BrandOverrides);
        foreach (var (token, value) in GraphicsPaletteMigration.AppearanceTokens(custom.Style)) Assert.Equal(value, resolved[token]);
        var compatibility = custom with { TemplateId = "compact-solid" };
        Assert.Same(compatibility, GraphicsPaletteMigration.MigrateRow(compatibility, GraphicsPalette.Default));
    }

    // ---- persistence ----------------------------------------------------------------------
    [Fact] public async Task ShowPaletteRoundTripsAndAnInvalidOneIsNeverWritten()
    {
        var path = Path.Combine(_directory, "show.json"); var store = new GraphicsDocumentStore(path);
        await store.LoadAsync();
        var palette = GraphicsPalette.Default.With(GraphicsPalette.TeamA, "#C8102EFF");
        await store.SaveAsync(new GraphicsShowDocument { Palette = palette });
        var loaded = (await new GraphicsDocumentStore(path).LoadAsync()).Document;
        Assert.Equal(palette.Colors, loaded.Palette!.Colors);
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(new GraphicsShowDocument { Palette = palette.With(GraphicsPalette.Accent, "red") }));
        // A pre-palette document writes no palette key at all.
        await store.SaveAsync(new GraphicsShowDocument());
        Assert.DoesNotContain("\"palette\"", await File.ReadAllTextAsync(path));
    }

    [Fact] public async Task LegacyShowLoadsOnTheAppDefaultSeededFromTheBrandKitAndKeepsRowColours()
    {
        var path = Path.Combine(_directory, "show.json");
        var custom = new GraphicsPlaylistRow { Id = "title", Name = "Title", TemplateId = "centered-title", TemplateVersion = 1,
            Style = new LowerThirdAppearance { AccentColor = "#E63946" } };
        await new GraphicsDocumentStore(path).SaveAsync(new GraphicsShowDocument { Playlist = [custom] });
        var session = new GraphicsWorkspaceSession((_, _, _) => Task.FromResult(new NativeGraphicsOperationResult()), () => null);
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(path), session);
        var defaults = new GraphicsPaletteDefaultsStore(Path.Combine(_directory, "palette-default.json"));
        library.UsePaletteDefaults(defaults, () => GraphicsPaletteMigration.FromBrandKit("#13315C", "#E63946", "#0B1F3A", stock: false));
        await library.LoadAsync();
        Assert.True(library.PaletteMigrated);
        Assert.Equal("#0B1F3AE6", library.EffectivePalette.Colors[GraphicsPalette.Primary]);
        Assert.Equal("#0B1F3AE6", defaults.Load()!.Colors[GraphicsPalette.Primary]); // seeded once, on disk
        Assert.Same(library.EffectivePalette, session.Palette);
        var row = library.Document.Playlist.Single();
        var resolved = library.EffectivePalette.Resolve(row.BrandOverrides);
        foreach (var (token, value) in GraphicsPaletteMigration.AppearanceTokens(custom.Style)) Assert.Equal(value, resolved[token]);
        Assert.False(row.BrandOverrides!.ContainsKey(GraphicsPalette.Accent)); // already the palette's accent: linked
        // Saving the palette persists the migration with it.
        Assert.True(await library.SavePaletteAsync(library.EffectivePalette.With(GraphicsPalette.Accent, "#D4A017FF")), library.Status);
        var reloaded = (await new GraphicsDocumentStore(path).LoadAsync()).Document;
        Assert.Equal("#D4A017FF", reloaded.Palette!.Colors[GraphicsPalette.Accent]);
        Assert.NotNull(reloaded.Playlist.Single().BrandOverrides);
        // A new show is not migrated twice and the app default stays put.
        Assert.True(await library.SaveAppDefaultPaletteAsync(GraphicsPalette.Default), library.Status);
        Assert.Equal(GraphicsPalette.Default.Colors, defaults.Load()!.Colors);
        Assert.Equal("#D4A017FF", library.EffectivePalette.Colors[GraphicsPalette.Accent]);
    }

    [Fact] public void GatewayAcceptsOnlyResolvedPaletteShapes()
    {
        static JsonElement E(string json) => JsonDocument.Parse(json).RootElement;
        Assert.True(GraphicsCommandRequest.ValidPalette(E("null")));
        Assert.True(GraphicsCommandRequest.ValidPalette(E("{\"brand.accent\":\"#E63946FF\",\"brand.teamA\":\"#C8102EFF\"}")));
        Assert.False(GraphicsCommandRequest.ValidPalette(E("{\"brand.accent\":\"#E63946\"}")));
        Assert.False(GraphicsCommandRequest.ValidPalette(E("{\"brand.onPrimary\":\"auto\"}")));
        Assert.False(GraphicsCommandRequest.ValidPalette(E("{\"brand.fontBody\":\"#E63946FF\"}")));
        Assert.False(GraphicsCommandRequest.ValidPalette(E("[\"#E63946FF\"]")));
    }

    // ---- live re-theme ----------------------------------------------------------------------
    private readonly List<(string Action, JsonElement Body)> _commands = [];
    private NativeGraphicsState _state = new() { CoreEpoch = "core-a", SchemaVersion = 1, Templates = [
        new() { TemplateId = "centered-title", TemplateVersion = 1, Name = "Centered title", Family = "title", Runtime = "declarative", ManifestHash = new string('a', 64),
            Definition = JsonSerializer.SerializeToElement(new { templateId = "centered-title", templateVersion = 1 }),
            Fields = [new() { Id = "heading", Type = "string", Default = JsonSerializer.SerializeToElement("") }] },
        new() { TemplateId = "compact-solid", TemplateVersion = 1, Name = "Compact", Family = "lower-third", Runtime = "compatibility", ManifestHash = new string('b', 64) }] };
    private Task<NativeGraphicsOperationResult> Invoke(string action, string body, CancellationToken _)
    {
        var input = JsonDocument.Parse(body).RootElement.Clone(); _commands.Add((action, input));
        var id = input.TryGetProperty("instanceId", out var raw) ? raw.GetString()! : ""; long revision = 1;
        JsonElement? Palette(NativeGraphicsInstance i) => input.TryGetProperty("palette", out var p) ? (p.ValueKind == JsonValueKind.Null ? null : p.Clone()) : i.Palette;
        NativeGraphicsInstance[] Map(Func<NativeGraphicsInstance, NativeGraphicsInstance> change) => _state.Instances.Select(i => i.InstanceId == id ? change(i) : i).ToArray();
        switch (action) {
            case "graphics.instance.create":
                _state = _state with { Instances = [.. _state.Instances, new() { InstanceId = id, Revision = 1, TemplateId = input.GetProperty("templateId").GetString()!, TemplateVersion = 1,
                    Animations = JsonSerializer.Deserialize<NativeGraphicsAnimations>(input.GetProperty("animations"), Json)!,
                    Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), Json)!,
                    Palette = input.TryGetProperty("palette", out var created) && created.ValueKind == JsonValueKind.Object ? created.Clone() : null }] };
                break;
            case "graphics.instance.update":
                var program = input.GetProperty("target").GetString() == "program";
                revision = _state.Instances.Single(i => i.InstanceId == id).Revision + 1;
                _state = _state with { Instances = Map(i => i with { Revision = revision, Palette = Palette(i),
                    Fields = input.TryGetProperty("fields", out var f) ? JsonSerializer.Deserialize<NativeGraphicsFields>(f, Json)! : i.Fields,
                    ProgramRevision = program ? revision : i.ProgramRevision, ProgramPalette = program ? Palette(i) : i.ProgramPalette }) };
                break;
            case "graphics.instance.cue":
                _state = _state with { Instances = Map(i => i with { CueRevision = input.GetProperty("expectedRevision").GetInt64(), Cued = true, CuePalette = i.Palette,
                    RideTake = !input.TryGetProperty("rideTake", out var ride) || ride.GetBoolean() }) };
                break;
            case "graphics.instance.trigger":
                _state = _state with { Instances = Map(i => i with { ProgramRevision = i.CueRevision, ProgramPalette = i.CuePalette, Visibility = "visible", CueRevision = 0, Cued = false, CuePalette = null }) };
                break;
        }
        return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = revision, Reason = "accepted" });
    }
    private async Task<(GraphicsWorkspaceLibrary, GraphicsPlayoutService, GraphicsPlaylistRow)> Playout()
    {
        var session = new GraphicsWorkspaceSession(Invoke, () => _state); session.Observe(_state);
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(_directory, "show.json")), session);
        var row = new GraphicsPlaylistRow { Id = "title", Kind = GraphicsPlaylistRow.Title, Name = "Show title", TemplateId = "centered-title", TemplateVersion = 1,
            Fields = new Dictionary<string, JsonElement> { ["heading"] = JsonSerializer.SerializeToElement("Hello") } };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        return (library, new GraphicsPlayoutService(library, Invoke, () => _state, (_, _) => Task.CompletedTask, () => DateTime.UtcNow), library.Document.Playlist.Single());
    }
    private static string Accent(JsonElement? palette) => palette!.Value.GetProperty(GraphicsPalette.Accent).GetString()!;

    [Fact] public async Task PlayoutSendsTheResolvedPaletteAndRowOverrides()
    {
        var (library, service, row) = await Playout();
        Assert.True(await library.SavePlaylistAsync([row with { BrandOverrides = new Dictionary<string, string> { [GraphicsPalette.Accent] = "#E63946FF" } }]), library.Status);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(row))).Accepted);
        var sent = _commands.Single(c => c.Action == "graphics.instance.create").Body.GetProperty("palette");
        Assert.Equal("#E63946FF", sent.GetProperty(GraphicsPalette.Accent).GetString());
        Assert.Equal("#172433E6", sent.GetProperty(GraphicsPalette.Primary).GetString());
        Assert.Empty(service.PaletteStaleOnAir());
    }

    [Fact] public async Task PaletteEditRecoloursCueNowAndProgramOnlyOnUpdateOnAir()
    {
        var (library, service, row) = await Playout();
        var entry = GraphicsPlayoutService.EntryFor(row);
        Assert.True((await service.PlayInAsync(entry)).Accepted); // on Program with the default accent
        Assert.True((await service.ToPreviewAsync(entry)).Accepted); // and cued
        Assert.True(await library.SavePaletteAsync(library.EffectivePalette.With(GraphicsPalette.Accent, "#D4A017FF")), library.Status);
        _commands.Clear();
        var refreshed = await service.RefreshPaletteAsync();
        Assert.True(refreshed.Accepted, refreshed.Reason);
        var instance = _state.Instances.Single();
        Assert.Equal("#D4A017FF", Accent(instance.CuePalette)); // Preview follows at once
        Assert.Equal("#44C1A1FF", Accent(instance.ProgramPalette)); // Program does not
        Assert.Equal("cue", _commands.First(c => c.Action == "graphics.instance.update").Body.GetProperty("target").GetString());
        Assert.Single(service.PaletteStaleOnAir());
        // The cue carries a newer draft, so Update on air names it instead of sending its other edits to Program.
        var skipped = await service.UpdateOnAirAsync();
        Assert.False(skipped.Accepted); Assert.Contains("Show it from Preview", skipped.Reason);
        Assert.Equal("#44C1A1FF", Accent(_state.Instances.Single().ProgramPalette));
        // With nothing cued, Update on air is a palette-only Program update.
        _state = _state with { Instances = _state.Instances.Select(i => i with { CueRevision = 0, Cued = false, Revision = i.ProgramRevision }).ToArray() };
        _commands.Clear();
        var updated = await service.UpdateOnAirAsync();
        Assert.True(updated.Accepted, updated.Reason);
        var body = _commands.Single().Body;
        Assert.Equal("program", body.GetProperty("target").GetString());
        Assert.False(body.TryGetProperty("fields", out _));
        Assert.Equal("#D4A017FF", Accent(_state.Instances.Single().ProgramPalette));
        Assert.Empty(service.PaletteStaleOnAir());
    }

    [Fact] public async Task CompatibilityLooksSendNoPalette()
    {
        var (library, service, row) = await Playout();
        var compat = row with { TemplateId = "compact-solid", Fields = new Dictionary<string, JsonElement> { ["name"] = JsonSerializer.SerializeToElement("Ada") } };
        Assert.True(await library.SavePlaylistAsync([compat]), library.Status);
        Assert.True((await service.EnsureInstanceAsync(GraphicsPlayoutService.EntryFor(compat))).Accepted);
        Assert.False(_commands.Single(c => c.Action == "graphics.instance.create").Body.TryGetProperty("palette", out _));
        Assert.Empty(service.PaletteStaleOnAir());
    }
}
