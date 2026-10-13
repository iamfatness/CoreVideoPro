using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Immediate, selected-member edits. Stored pin rectangles remain normalized within the wall.</summary>
public sealed class SceneWorkspaceMemberEditor(StudioViewModel studio, SceneWorkspaceViewModel workspace) : ObservableObject
{
    private string Id => workspace.SelectedMember?.Id ?? string.Empty;
    private DynamicGallerySettings? Wall => studio.PreviewScene.DynamicGallery;
    private TilesMemberOverride Value => Wall?.Overrides.GetValueOrDefault(Id)?.Clone() ?? new();
    public bool CanEdit => workspace.CanEditSelected && workspace.IsWallSelected && workspace.SelectedMember?.IsAvailable==true && Id.Length > 0;
    public string FramingTitle => (workspace.SelectedMember?.Name ?? "Select a member") + " · framing";
    public bool NeverShow { get => studio.WorkspaceMemberExcluded(Id); set { if (!CanEdit || value == NeverShow) return; workspace.Checkpoint(); studio.SetTilesExclusion(Id, value); Refresh(); } }
    public bool Pinned { get => Value.Rect is not null; set { if (!CanEdit || value == Pinned) return; workspace.Checkpoint(); Edit(v => {v.Rect = value ? new() { Width=.48, Height=1 } : null;if(!value)v.Z=null;}); } }
    public double CropLeft { get => Value.CropLeftPercent; set => Crop(value, true); }
    public double CropRight { get => Value.CropRightPercent; set => Crop(value, false); }
    public double WallWidth => Math.Max(1, (Wall?.WallRect.Width ?? 1) * (workspace.Selected?.CanvasPixelWidth ?? 1920));
    public double WallHeight => Math.Max(1, (Wall?.WallRect.Height ?? 1) * (workspace.Selected?.CanvasPixelHeight ?? 1080));
    public double PixelX { get => (Value.Rect?.X ?? 0) * WallWidth; set => Pin(value, 'x'); }
    public double PixelY { get => (Value.Rect?.Y ?? 0) * WallHeight; set => Pin(value, 'y'); }
    public double PixelWidth { get => (Value.Rect?.Width ?? .48) * WallWidth; set => Pin(value, 'w'); }
    public double PixelHeight { get => (Value.Rect?.Height ?? 1) * WallHeight; set => Pin(value, 'h'); }
    public double Order {get=>Value.Z??0;set{if(CanEdit&&Pinned&&double.IsFinite(value)&&value>=0&&value<=63&&value==Math.Truncate(value))Edit(v=>v.Z=(int)value);}}
    private void Crop(double value, bool left)
    {
        if (!CanEdit || !double.IsFinite(value) || value < 0 || value > 45) return;
        if (value == (left ? CropLeft : CropRight)) return;
        Edit(v => { if(left) v.CropLeftPercent=value; else v.CropRightPercent=value; });
    }
    public void ResetCrop()
    { if(!CanEdit)return; workspace.Checkpoint(); Edit(v => {v.CropLeftPercent=0;v.CropRightPercent=0;}); }
    private void Pin(double value, char field)
    {
        if(!CanEdit || !Pinned || !double.IsFinite(value))return;
        var rect=Value.Rect!;
        var normalized=value/(field is 'x' or 'w' ? WallWidth : WallHeight);
        if(field=='x')rect.X=normalized; else if(field=='y')rect.Y=normalized;
        else if(field=='w')rect.Width=normalized; else rect.Height=normalized;
        if(rect.X<0||rect.Y<0||rect.Width<=0||rect.Height<=0||rect.X+rect.Width>1.00000001||rect.Y+rect.Height>1.00000001)
        {studio.CommandStatus="The tile position and size must fit inside this wall.";Refresh();return;}
        Edit(v=>v.Rect=rect);
    }
    private void Edit(Action<TilesMemberOverride> change)
    {
        var id=Id;
        studio.WorkspaceWallChange(w=> {var value=w.Overrides.GetValueOrDefault(id)?.Clone()??new();change(value);w.Overrides[id]=value;});
        Refresh();
    }
    public void Refresh()
    {
        foreach(var p in new[]{nameof(CanEdit),nameof(FramingTitle),nameof(NeverShow),nameof(Pinned),nameof(CropLeft),nameof(CropRight),nameof(WallWidth),nameof(WallHeight),nameof(PixelX),nameof(PixelY),nameof(PixelWidth),nameof(PixelHeight),nameof(Order)})OnPropertyChanged(p);
    }
}
