using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.WinUI.ViewModels;
public sealed partial class StudioViewModel
{
    private readonly Dictionary<string,GradeScopeRoi> _scopePreferences=new(StringComparer.Ordinal);
    private Dictionary<string,GradeScopeRoi> CaptureScopePreferences() => new(_scopePreferences,StringComparer.Ordinal);
    private void RestoreScopePreferences(Dictionary<string,GradeScopeRoi>? values)
    {
        _scopePreferences.Clear();foreach(var p in (values??[]).Take(64))
            if ((p.Key.StartsWith("capture:",StringComparison.Ordinal)||p.Key.StartsWith("zoom-person:",StringComparison.Ordinal)) && p.Value?.IsValid==true)
                _scopePreferences[p.Key]=p.Value with { Revision=0 };
    }
    private void InitializeEditorScopePreferences(ColorGradeEditorViewModel editor)
    {
        // Capture the stable identity once. Never save a departed guest's region
        // under whoever later reused this source's transient meeting identifier.
        var key=GradePersistenceKey(editor.SourceId);var binding=GradeSessionBinding(editor.SourceId);
        if (key is not null && _scopePreferences.TryGetValue(key,out var roi)) editor.SetScopeRoi(roi);
        editor.PropertyChanged+=(_,e)=> {
            if (e.PropertyName!=nameof(editor.ScopeRoi) || key is null || GradeSessionBinding(editor.SourceId)!=binding) return;
            _scopePreferences[key]=editor.ScopeRoi with { Revision=0 };ScheduleGradePersistence();
        };
    }
}
