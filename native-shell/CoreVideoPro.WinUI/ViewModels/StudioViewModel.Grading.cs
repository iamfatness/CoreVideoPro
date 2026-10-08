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
    private void StopGradePreviewsForShutdown()
    {
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

        var sourceName = ResolveColorGradeSourceName(normalizedSourceId);

        var seed = ResolveStoredColorGrade(normalizedSourceId);
        var editorViewModel = new ColorGradeEditorViewModel(
            normalizedSourceId,
            sourceName,
            seed);
        editorViewModel.GradeChanged += OnSourceColorGradeChanged;
        editorViewModel.GradeSaved += OnSourceColorGradeSaved;

        var window = new ColorGradeEditorWindow(editorViewModel);
        var preview = new GradePreviewCoordinator(_bridge, editorViewModel, RunOnUiThread);
        window.Closed += (_, _) =>
        {
            preview.Dispose();
            _gradePreviews.Remove(editorViewModel);
            editorViewModel.GradeChanged -= OnSourceColorGradeChanged;
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
        ApplyColorGradeToMatchingRoutes(editorViewModel.SourceId, grade);
        CommandStatus = status;

        SyncPreviewCanvasLayers(GetPreviewEditableRoutes());
        RefreshPreviewRoutingState();
        _ = SyncColorGradeChangeAsync();
    }

    private async Task SyncColorGradeChangeAsync()
    {
        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            RunOnUiThread(() => CommandStatus = ex.Message);  // catch runs off-thread (ConfigureAwait(false))
        }
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

    private ColorGrade ResolveStoredColorGrade(string sourceId) =>
        _sourceColorGrades.TryGetValue(sourceId, out var stored) ? stored : ColorGrade;

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
            grade = stored;
        }

        return grade is null
            ? null
            : new MediaCoreColorGradeWire(
                grade.Lut,
                grade.Exposure,
                grade.Contrast,
                grade.Saturation,
                grade.Temperature);
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
