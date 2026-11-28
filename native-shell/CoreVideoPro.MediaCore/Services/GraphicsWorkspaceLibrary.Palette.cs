using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>The app default palette (owner ruling 2026-11-27: palette per show, seeded from an
/// app-wide default for new shows). One small JSON file beside the graphics document; written
/// atomically; a missing or unreadable file is reseeded, never fatal.</summary>
public sealed class GraphicsPaletteDefaultsStore
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web) { WriteIndented = true, MaxDepth = 8 };
    private readonly string _path;
    public GraphicsPaletteDefaultsStore(string path) => _path = Path.GetFullPath(path);

    /// <summary>The stored default, or null when there is none or it is not a valid palette.</summary>
    public GraphicsPalette? Load()
    {
        try {
            if (!File.Exists(_path) || new FileInfo(_path).Length > 64 * 1024) return null;
            var palette = JsonSerializer.Deserialize<GraphicsPalette>(File.ReadAllBytes(_path), Options);
            return palette is { IsValid: true } ? palette : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or NotSupportedException) { return null; }
    }

    public void Save(GraphicsPalette palette)
    {
        if (!palette.IsValid) throw new InvalidDataException("Invalid palette.");
        var directory = Path.GetDirectoryName(_path)!; Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, ".palette-" + Guid.NewGuid().ToString("N") + ".tmp");
        try {
            File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(palette, Options));
            File.Move(temporary, _path, true);
        } finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}

public sealed partial class GraphicsWorkspaceLibrary
{
    private GraphicsPaletteDefaultsStore? _paletteDefaults;
    private Func<GraphicsPalette>? _paletteSeed;

    /// <summary>The app default every new show starts from. Seeded once from the legacy brand kit.</summary>
    public GraphicsPalette AppDefaultPalette { get; private set; } = GraphicsPalette.Default;
    /// <summary>The palette this show resolves against: its own, else the app default.</summary>
    public GraphicsPalette EffectivePalette => Document.Palette ?? AppDefaultPalette;
    /// <summary>True when the open show was written before the palette and was moved onto it in
    /// memory at load. The next save of anything persists the migration.</summary>
    public bool PaletteMigrated { get; private set; }

    /// <summary>Call before the first load. <paramref name="seed"/> builds the app default when the
    /// store has none (the shell passes <see cref="GraphicsPaletteMigration.FromBrandKit"/>).</summary>
    public void UsePaletteDefaults(GraphicsPaletteDefaultsStore store, Func<GraphicsPalette> seed) => (_paletteDefaults, _paletteSeed) = (store, seed);

    private Task<GraphicsShowDocument> AdoptPaletteAsync(GraphicsShowDocument document) => Task.Run(() => {
        if (_paletteDefaults is { } store) {
            var stored = store.Load();
            if (stored is null && _paletteSeed?.Invoke() is { IsValid: true } seeded) {
                stored = seeded;
                try { store.Save(seeded); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException) { /* Used in memory; reseeded next start. */ }
            }
            AppDefaultPalette = stored ?? GraphicsPalette.Default;
        }
        if (document.Palette is not null || ReadOnly) return document;
        // A pre-palette show: the palette starts from the app default, and every declarative row keeps
        // its exact colours as row overrides wherever they differ from what that palette resolves to.
        // A brand-new (empty) show simply starts on the app default; only a show with content was "migrated".
        PaletteMigrated = document.Playlist.Count > 0 || document.Cues.Count > 0 || document.Designs.Count > 0;
        var palette = AppDefaultPalette;
        return document with { Palette = palette, Playlist = document.Playlist.Select(r => GraphicsPaletteMigration.MigrateRow(r, palette)).ToArray() };
    });

    /// <summary>Save the show palette. Cue and Preview follow it at once through
    /// <see cref="GraphicsPlayoutService.RefreshPaletteAsync"/>; Program waits for Update on air.</summary>
    public async Task<bool> SavePaletteAsync(GraphicsPalette palette)
    {
        await LoadAsync();
        if (!palette.IsValid) return Fail("That palette has an invalid colour. Previous palette kept.");
        return await Save(Document with { Palette = palette }, "Palette saved. Cue and Preview use it now; Program changes only on Update on air.");
    }

    /// <summary>Make <paramref name="palette"/> the app default for new shows. The open show is unchanged.</summary>
    public async Task<bool> SaveAppDefaultPaletteAsync(GraphicsPalette palette)
    {
        await LoadAsync();
        if (!palette.IsValid) return Fail("That palette has an invalid colour. App default kept.");
        if (_paletteDefaults is null) return Fail("No app default store is configured.");
        try { await Task.Run(() => _paletteDefaults.Save(palette)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException) { return Fail("Could not save the app default: " + ex.Message); }
        AppDefaultPalette = palette; Status = "Saved as the app default for new shows. This show is unchanged."; Changed?.Invoke(this, EventArgs.Empty);
        return true;
    }
}
