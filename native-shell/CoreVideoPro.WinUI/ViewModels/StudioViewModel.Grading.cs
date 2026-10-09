using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.Views;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private readonly Dictionary<ColorGradeEditorViewModel, GradePreviewCoordinator> _gradePreviews = [];
    private readonly Dictionary<string, ColorGrade> _persistedSourceGrades = new(StringComparer.Ordinal);
    private readonly Dictionary<string, long> _sourceGradeRevisions = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _sourceGradeBindings = new(StringComparer.Ordinal);
    private string? GradePersistenceKey(string sourceId) => sourceId.StartsWith("capture:",StringComparison.Ordinal) ? sourceId :
        RoomVideoParticipants.FirstOrDefault(p=>p.Id==sourceId)?.PersistentId is { Length: > 0 } id ? $"zoom-person:{id}" : null;
    private string GradeSessionBinding(string sourceId) => sourceId.StartsWith("capture:",StringComparison.Ordinal) ? sourceId :
        RoomVideoParticipants.FirstOrDefault(p=>p.Id==sourceId) is { } p ? $"{p.PersistentId}:{p.SourceGeneration}" : "unavailable";
    private Dictionary<string,ColorGrade> CapturePersistedSourceGrades() => new(_persistedSourceGrades,StringComparer.Ordinal);
    private void RestorePersistedSourceGrades(Dictionary<string,ColorGrade>? grades)
    {
        _persistedSourceGrades.Clear();
        foreach(var item in (grades ?? []).Take(64)) {
            if(item.Value is null) continue;
            try { item.Value.Advanced?.Validate(); if(item.Key.StartsWith("capture:",StringComparison.Ordinal) || item.Key.StartsWith("zoom-person:",StringComparison.Ordinal)) _persistedSourceGrades[item.Key]=item.Value; }
            catch(ArgumentException ex) { CommandStatus=$"Saved grade unavailable: {ex.Message}"; }
        }
    }
    private bool _gradeHealthSubscribed;
    private void ObserveSourceGradeCoreLifetime() {
        if(_gradeHealthSubscribed) return;_gradeHealthSubscribed=true;_bridge.HealthChanged+=OnSourceGradeCoreHealth;
    }
    private void OnSourceGradeCoreHealth(MediaCoreHealth health) {
        if(health.Recovering || health.Stopped) RunOnUiThread(()=>_sourceGradeRevisions.Clear());
    }
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _gradePersistenceTimer;
    private void ScheduleGradePersistence()
    {
        _gradePersistenceTimer ??= _dispatcher.CreateTimer();
        _gradePersistenceTimer.Interval = TimeSpan.FromMilliseconds(750);
        _gradePersistenceTimer.IsRepeating = false;
        _gradePersistenceTimer.Tick -= OnGradePersistenceTick;
        _gradePersistenceTimer.Tick += OnGradePersistenceTick;
        _gradePersistenceTimer.Stop(); _gradePersistenceTimer.Start();
    }
    private void OnGradePersistenceTick(Microsoft.UI.Dispatching.DispatcherQueueTimer sender,object args) => SaveProductionOutputPreferences();
    private void StopGradePreviewsForShutdown()
    {
        if(_gradeHealthSubscribed) { _bridge.HealthChanged-=OnSourceGradeCoreHealth;_gradeHealthSubscribed=false; }
        _gradePersistenceTimer?.Stop();
        if (_gradePersistenceTimer is not null) { _gradePersistenceTimer.Tick -= OnGradePersistenceTick; SaveProductionOutputPreferences(); }

        foreach (var preview in _gradePreviews.Values) preview.Dispose();
        _gradePreviews.Clear();
    }

    /// <summary>
    /// Opens the per-source color grade pop-out for a Zoom participant or capture source.
    /// Saved grades are attached to matching scene routes and sent to native per layer.
    /// </summary>
    [RelayCommand]
    private void OpenColorGradeEditor(string? sourceId)
    {
        var normalizedSourceId = NormalizeColorGradeSourceId(sourceId);
        if (string.IsNullOrWhiteSpace(normalizedSourceId))
        {
            CommandStatus = "Select a source before editing its color grade";
            return;
        }

        if (_openColorGradeEditors.Count >= 3)
        {
            CommandStatus = "Close one of the three grade editors before opening another";
            return;
        }

        ObserveSourceGradeCoreLifetime();
        var sourceName = ResolveColorGradeSourceName(normalizedSourceId);

        var seed = ResolveStoredColorGrade(normalizedSourceId);
        var editorViewModel = new ColorGradeEditorViewModel(
            normalizedSourceId,
            sourceName,
            seed,
            _sourceGradeRevisions.GetValueOrDefault(normalizedSourceId));
        if (!_sourceColorGrades.ContainsKey(normalizedSourceId) && GradePersistenceKey(normalizedSourceId) is { } key && _persistedSourceGrades.ContainsKey(key)) {
            editorViewModel.LiveEditing = false;
            editorViewModel.EditStatus = "Saved grade loaded as a draft. Apply Live confirms this source binding.";
        }
        editorViewModel.ApplyGradeAsync = (grade,epoch,revision) => _bridge.ApplySourceGradeAsync(normalizedSourceId,epoch,revision,
            new(grade.Lut,grade.Exposure,grade.Contrast,grade.Saturation,grade.Temperature,grade.Advanced?.Copy()));
        editorViewModel.GradeChanged += OnSourceColorGradeChanged;
        editorViewModel.GradeAuthorityReset += OnSourceGradeAuthorityReset;
        editorViewModel.GradeSaved += OnSourceColorGradeSaved;

        var window = new ColorGradeEditorWindow(editorViewModel);
        var preview = new GradePreviewCoordinator(_bridge, editorViewModel, RunOnUiThread);
        window.Closed += (_, _) =>
        {
            preview.Dispose();
            editorViewModel.StopGradeEditing();
            _gradePreviews.Remove(editorViewModel);
            editorViewModel.GradeChanged -= OnSourceColorGradeChanged;
            editorViewModel.GradeAuthorityReset -= OnSourceGradeAuthorityReset;
            editorViewModel.GradeSaved -= OnSourceColorGradeSaved;
            _openColorGradeEditors.Remove(editorViewModel);
        };
        _gradePreviews.Add(editorViewModel, preview);
        _openColorGradeEditors.Add(editorViewModel);
        window.Activate();
    }

    [RelayCommand]
    private void OpenCaptureDeviceColorGradeEditor(string? captureDeviceId) =>
        OpenColorGradeEditor(string.IsNullOrWhiteSpace(captureDeviceId) ? null : $"capture:{captureDeviceId}");

    private void OnSourceGradeAuthorityReset(object? sender,EventArgs e)
    {
        if(sender is ColorGradeEditorViewModel editor) _sourceGradeRevisions.Remove(editor.SourceId);
    }

    private void OnSourceColorGradeSaved(object? sender, ColorGrade grade)
    {
        if (sender is not ColorGradeEditorViewModel editorViewModel)
        {
            return;
        }

        ApplyLiveColorGrade(editorViewModel, grade, $"Color grade set for {editorViewModel.SourceName}: {grade.Summary}");
    }

    private void OnSourceColorGradeChanged(object? sender, ColorGrade grade)
    {
        if (sender is not ColorGradeEditorViewModel editorViewModel)
        {
            return;
        }

        ApplyLiveColorGrade(editorViewModel, grade, $"Color grade live for {editorViewModel.SourceName}: {grade.Summary}");
    }

    private void ApplyLiveColorGrade(ColorGradeEditorViewModel editorViewModel, ColorGrade grade, string status)
    {
        _sourceColorGrades[editorViewModel.SourceId] = grade;
        _sourceGradeRevisions[editorViewModel.SourceId] = editorViewModel.AppliedRevision;
        _sourceGradeBindings[editorViewModel.SourceId] = GradeSessionBinding(editorViewModel.SourceId);
        if(GradePersistenceKey(editorViewModel.SourceId) is { } key) {
            _persistedSourceGrades.Remove(key);
            if(_persistedSourceGrades.Count>=64) { _persistedSourceGrades.Remove(_persistedSourceGrades.Keys.First()); status += " · oldest saved source grade retired (64-source limit)"; }
            _persistedSourceGrades[key]=grade;
        }
        ApplyColorGradeToMatchingRoutes(editorViewModel.SourceId, grade);
        CommandStatus = status;

        SyncPreviewCanvasLayers(GetPreviewEditableRoutes());
        RefreshPreviewRoutingState();
        ScheduleGradePersistence();
        // The native source-grade acknowledgement already owns live state.
    }

    private string? NormalizeColorGradeSourceId(string? sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId))
        {
            return null;
        }

        if (sourceId.StartsWith("input-", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(sourceId[6..], out var slotNumber) &&
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is { } slot)
        {
            return slot.Kind == ShowInputKind.ZoomParticipant
                ? slot.ParticipantId
                : string.IsNullOrWhiteSpace(slot.CaptureDeviceId) ? null : $"capture:{slot.CaptureDeviceId}";
        }

        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureDeviceId = sourceId["capture:".Length..];
            return string.IsNullOrWhiteSpace(captureDeviceId) ? null : $"capture:{captureDeviceId}";
        }

        if (CaptureDevices.Any(device => string.Equals(device.Id, sourceId, StringComparison.Ordinal)))
        {
            return $"capture:{sourceId}";
        }

        return sourceId;
    }

    private string ResolveColorGradeSourceName(string sourceId)
    {
        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureDeviceId = sourceId["capture:".Length..];
            return CaptureDevices.FirstOrDefault(device => string.Equals(device.Id, captureDeviceId, StringComparison.Ordinal))?.Name ??
                captureDeviceId;
        }

        return RoomVideoParticipants.FirstOrDefault(participant => participant.Id == sourceId)?.Name ?? sourceId;
    }

    private ColorGrade ResolveStoredColorGrade(string sourceId)
    {
        if(_sourceColorGrades.TryGetValue(sourceId,out var stored) && _sourceGradeBindings.TryGetValue(sourceId,out var binding) && binding==GradeSessionBinding(sourceId)) return stored;
        _sourceColorGrades.Remove(sourceId);
        _sourceGradeRevisions.Remove(sourceId);
        return GradePersistenceKey(sourceId) is { } key && _persistedSourceGrades.TryGetValue(key,out stored) ? stored : ColorGrade;
    }

    private void ApplyColorGradeToMatchingRoutes(string sourceId, ColorGrade grade)
    {
        foreach (var route in _sceneRoutes.Values.SelectMany(routes => routes))
        {
            var resolved = ResolveRouteFromShowInput(route);
            if (string.Equals(ResolveColorGradeSourceId(resolved), sourceId, StringComparison.Ordinal))
            {
                route.ColorGrade = grade;
            }
        }
    }

    private MediaCoreColorGradeWire? BuildRouteColorGradeWire(SourceRoute route)
    {
        var sourceId = ResolveColorGradeSourceId(route);
        var grade = route.ColorGrade;
        if (sourceId is not null && _sourceColorGrades.TryGetValue(sourceId, out var stored))
        {
            grade = _sourceGradeBindings.TryGetValue(sourceId,out var binding) && binding == GradeSessionBinding(sourceId) ? stored : new ColorGrade { Lut = "none" };
        }

        return grade is null
            ? null
            : new MediaCoreColorGradeWire(
                grade.Lut,
                grade.Exposure,
                grade.Contrast,
                grade.Saturation,
                grade.Temperature,
                grade.Advanced?.Copy());
    }

    private static string? ResolveColorGradeSourceId(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice && route.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            return $"capture:{captureDeviceId}";
        }

        return route.ParticipantId;
    }

}
