using CommunityToolkit.Mvvm.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private string _gallerySelectedMemberId = string.Empty;
    private double _galleryManualSlot = 1;
    [ObservableProperty] private double _galleryTileX;
    [ObservableProperty] private double _galleryTileY;
    [ObservableProperty] private double _galleryTileWidth = .5;
    [ObservableProperty] private double _galleryTileHeight = 1;
    [ObservableProperty] private double _galleryTileCropLeft;
    [ObservableProperty] private double _galleryTileCropRight;
    [ObservableProperty] private double _galleryTileOrder;
    private string? _galleryEditorSceneId;
    private string? _galleryEditorSourceId;
    private void LoadGalleryTileEditor(bool force = false)
    {
        if (!force && _galleryEditorSceneId == PreviewSceneId && _galleryEditorSourceId == GallerySelectedMemberId) return;
        _galleryEditorSceneId = PreviewSceneId;
        _galleryEditorSourceId = GallerySelectedMemberId;
        var value = TilesOverridePolicy.EditorValues(PreviewScene.DynamicGallery, GallerySelectedMemberId);
        GalleryTileX = value.Rect?.X ?? 0;
        GalleryTileY = value.Rect?.Y ?? 0;
        GalleryTileWidth = value.Rect?.Width ?? .5;
        GalleryTileHeight = value.Rect?.Height ?? 1;
        GalleryTileCropLeft = value.CropLeftPercent;
        GalleryTileCropRight = value.CropRightPercent;
        GalleryTileOrder = value.Z ?? 0;
    }
    public string GalleryBackgroundColor { get => PreviewScene.DynamicGallery?.BackgroundColor ?? "#000000";
        set => UpdateGallery(s => s.BackgroundColor = SceneRoutingService.NormalizeBorderColor(value)); }
    public void SetTilesBackground(string color, string sourceId)
    {
        RequireTilesPreview();
        if (!string.IsNullOrEmpty(sourceId) && !GalleryMemberChoices.Any(p => p.Id == sourceId))
            throw new ArgumentException("Choose an available background source.");
        UpdateGallery(s => { s.BackgroundColor = SceneRoutingService.NormalizeBorderColor(color); s.BackgroundSourceId = sourceId; });
    }
    public void SetTilesOverride(string sourceId, double x, double y, double width, double height, double left, double right, double z)
    {
        RequireTilesPreview();
        if (!GalleryMemberChoices.Any(p => p.Id == sourceId)) throw new ArgumentException("Choose an available tile source.");
        var item = TilesOverridePolicy.Create(x, y, width, height, left, right, z);
        UpdateGallery(s => s.Overrides[sourceId] = item);
        LoadGalleryTileEditor(force: true);
    }
    public void ClearTilesOverride(string sourceId)
    {
        RequireTilesPreview();
        UpdateGallery(s => s.Overrides.Remove(sourceId));
        LoadGalleryTileEditor(force: true);
    }
    [RelayCommand] private void ApplyGalleryTileOverride() => TryTilesEdit(() => SetTilesOverride(GallerySelectedMemberId,
        GalleryTileX, GalleryTileY, GalleryTileWidth, GalleryTileHeight, GalleryTileCropLeft, GalleryTileCropRight, GalleryTileOrder));
    [RelayCommand] private void ClearGalleryTileOverride() => TryTilesEdit(() => ClearTilesOverride(GallerySelectedMemberId));
    [RelayCommand] private void SetGalleryBackgroundSource() => TryTilesEdit(() => SetTilesBackground(GalleryBackgroundColor, GallerySelectedMemberId));
    [RelayCommand] private void ClearGalleryBackgroundSource() => TryTilesEdit(() => SetTilesBackground(GalleryBackgroundColor, ""));
    // SelectedValue can be null while the source list is refreshed during Take.
    [System.Diagnostics.CodeAnalysis.AllowNull]
    public string GallerySelectedMemberId { get => _gallerySelectedMemberId; set { if (SetProperty(ref _gallerySelectedMemberId, value ?? string.Empty)) LoadGalleryTileEditor(); } }
    public double GalleryManualSlot { get => _galleryManualSlot; set => SetProperty(ref _galleryManualSlot, double.IsFinite(value) ? Math.Clamp(Math.Round(value), 1, 64) : 1); }
    public IReadOnlyList<GalleryMemberChoice> GalleryMemberChoices => RoomVideoParticipants
        .Select(p => new GalleryMemberChoice(p.Id.Contains(':') ? p.Id : "zoom:" + p.Id, p.Name)).ToList();
    public IReadOnlyList<RouteSelectOption> GalleryMembershipModeOptions { get; } =
    [
        new() { Value = "routed", Label = "Routed show sources only" },
        new() { Value = "eligible", Label = "All eligible meeting participants" },
        new() { Value = "manual", Label = "Manual slots only" }
    ];
    public string GalleryMembershipMode
    {
        get => TilesMembershipPolicy.NormalizeMode(PreviewScene.DynamicGallery?.MembershipMode);
        set => UpdateGallery(settings =>
        {
            settings.MembershipMode = TilesMembershipPolicy.NormalizeMode(value);
            settings.AutoFill = settings.MembershipMode != "manual";
        });
    }
    public string GalleryMembershipSummary
    {
        get
        {
            var settings = PreviewScene.DynamicGallery;
            if (settings is null) return string.Empty;
            string Name(string id) => GalleryMemberChoices.FirstOrDefault(p => p.Id == id)?.Name ?? "Unavailable source";
            var slots = settings.ManualSlots.Select((id, index) => string.IsNullOrEmpty(id) ? null : $"{index + 1}: {Name(id)}").Where(s => s is not null);
            var exclusions = settings.ExcludedSourceIds.Select(Name);
            return "Manual slots: " + string.Join(", ", slots) + " · Never show: " + string.Join(", ", exclusions);
        }
    }
    public void SetTilesAutoFill(bool enabled)
    {
        RequireTilesPreview();
        GalleryMembershipMode = enabled ? "eligible" : "manual";
    }
    public void AssignTilesSlot(int slot, string sourceId)
    {
        RequireTilesPreview();
        if (slot is < 1 or > 64) throw new ArgumentOutOfRangeException(nameof(slot), "Tiles slot must be 1–64.");
        if (!string.IsNullOrEmpty(sourceId) && !GalleryMemberChoices.Any(p => p.Id == sourceId))
            throw new ArgumentException("Choose an available source for this Tiles slot.");
        UpdateGallery(settings =>
        {
            while (settings.ManualSlots.Count < slot) settings.ManualSlots.Add(null);
            for (var index = 0; index < settings.ManualSlots.Count; index++)
                if (settings.ManualSlots[index] == sourceId) settings.ManualSlots[index] = null;
            settings.ManualSlots[slot - 1] = string.IsNullOrEmpty(sourceId) ? null : sourceId;
        });
    }
    public void SetTilesExclusion(string sourceId, bool excluded)
    {
        RequireTilesPreview();
        if (string.IsNullOrWhiteSpace(sourceId)) throw new ArgumentException("Choose a source to exclude or allow.");
        if (excluded && !GalleryMemberChoices.Any(p => p.Id == sourceId)) throw new ArgumentException("Choose an available source to exclude.");
        UpdateGallery(settings =>
        {
            settings.ExcludedSourceIds.RemoveAll(id => id == sourceId);
            if (excluded) settings.ExcludedSourceIds.Add(sourceId);
        });
    }
    private void RequireTilesPreview()
    {
        if (PreviewScene.DynamicGallery is null) throw new InvalidOperationException("Queue a CoreVideo Tiles scene in Preview first.");
    }
    private void TryTilesEdit(Action edit)
    {
        try { edit(); } catch (Exception ex) { CommandStatus = ex.Message; }
    }
    [RelayCommand] private void AssignGalleryMember() => TryTilesEdit(() => AssignTilesSlot((int)GalleryManualSlot, GallerySelectedMemberId));
    [RelayCommand] private void ClearGallerySlot() => TryTilesEdit(() => AssignTilesSlot((int)GalleryManualSlot, string.Empty));
    [RelayCommand] private void ExcludeGalleryMember() => TryTilesEdit(() => SetTilesExclusion(GallerySelectedMemberId, true));
    [RelayCommand] private void AllowGalleryMember() => TryTilesEdit(() => SetTilesExclusion(GallerySelectedMemberId, false));
    public double GalleryCornerRadius
    {
        get => PreviewScene.DynamicGallery?.CornerRadius ?? 16;
        set => UpdateGallery(settings => settings.CornerRadius = double.IsFinite(value) ? Math.Clamp(value, 0, 100) : 16);
    }
}

public sealed record GalleryMemberChoice(string Id, string Name);
