using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public sealed record WorkspaceMemberState(bool Selected,bool CanSelect,bool Excluded,bool Pinned);

/// <summary>Editor intent, independent of admission freshness and the visible-tile ceiling.</summary>
public static class SceneWorkspaceMembershipPolicy
{
    public static IReadOnlyDictionary<string,WorkspaceMemberState> Build(DynamicGallerySettings? wall,
        IEnumerable<string> members,IReadOnlySet<string> routed,IReadOnlyList<TilesIdentityPolicy.Person> roster,string? meeting)
    {
        if(wall is null)return new Dictionary<string,WorkspaceMemberState>();
        string? Resolve(string? saved)=>TilesIdentityPolicy.ResolveLiveSourceId(saved,roster,meeting,wall.BoundMeetingId);
        HashSet<string> Keys(IEnumerable<string?> keys)=>keys.Select(Resolve).Where(k=>!string.IsNullOrEmpty(k)).Select(k=>k!).ToHashSet(StringComparer.Ordinal);
        var excluded=Keys(wall.ExcludedSourceIds);var omitted=Keys(wall.OmittedSourceIds);var manual=Keys(wall.ManualSlots);
        var mode=TilesMembershipPolicy.NormalizeMode(wall.MembershipMode);
        return members.Distinct(StringComparer.Ordinal).ToDictionary(id=>id,id=>
        {
            var blocked=excluded.Contains(id);
            var selected=!blocked&&(mode=="eligible"||!omitted.Contains(id)&&(mode=="routed"?routed.Contains(id):manual.Contains(id)));
            var editable=!blocked&&(mode=="manual"||mode=="routed"&&routed.Contains(id));
            return new WorkspaceMemberState(selected,editable,blocked,wall.Overrides.GetValueOrDefault(id)?.Rect is not null);
        },StringComparer.Ordinal);
    }
}
