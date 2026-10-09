using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using System.Collections.ObjectModel;
using System.Text.Json;
using CoreVideoPro.WinUI.Models;
using System.Globalization;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class ColorGradeEditorViewModel
{
    private AdvancedGradeDocument? GradeDocument;
    private bool _restoringGrade;
    private static ColorGrade CopyGrade(ColorGrade grade) => new() { Lut=grade.Lut, Exposure=grade.Exposure, Contrast=grade.Contrast, Saturation=grade.Saturation, Temperature=grade.Temperature, Advanced=grade.Advanced?.Copy() };
    private void RememberGrade() { if (_restoringGrade) return; _undo.Push(CopyGrade(CurrentGrade)); if (_undo.Count > 100) TrimHistory(); _redo.Clear(); }
    private void RestoreGrade(ColorGrade grade) {
        _restoringGrade = true;
        try { Lut = grade.Lut; Exposure = grade.Exposure; Contrast = grade.Contrast; Saturation = grade.Saturation; Temperature = grade.Temperature; SetDocument(grade.Advanced?.Copy()); }
        finally { _restoringGrade = false; }
        OnGradeEdited();
    }
    private readonly Stack<ColorGrade> _undo = new(), _redo = new();
    public ObservableCollection<GradeOperation> Adjustments { get; } = [];
    [ObservableProperty] private bool _advancedExpanded;
    [ObservableProperty] private bool _scopesEnabled = true;
    [ObservableProperty] private bool _scopesOriginal;
    [ObservableProperty] private int _histogramMode = 1;
    [ObservableProperty] private int _waveformMode = 1;
    [ObservableProperty] private int _scopeView;
    partial void OnScopeViewChanged(int value) => RequestPreview();
    partial void OnScopesEnabledChanged(bool value) => RequestPreview();
    partial void OnScopesOriginalChanged(bool value) => RequestPreview();
    partial void OnHistogramModeChanged(int value) => RequestPreview();
    partial void OnWaveformModeChanged(int value) => RequestPreview();
    [ObservableProperty] private GradeOperation? _selectedAdjustment;
    [ObservableProperty] private string _editStatus = "Basic controls retain their established scale.";
    [ObservableProperty] private string _scopeStatus = "Scopes hidden.";
    [ObservableProperty] private VideoSurfaceState _scopeSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,"grade:scopes","Scopes");
    private void ObserveScopes(GradePreviewObservation observation)
    {
        if (!AdvancedExpanded || !ScopesEnabled) { ScopeStatus = "Scopes hidden."; return; }
        var s = observation.Scopes;
        if (s is null || s.Revision != Revision || s.SourceEpoch != observation.SourceEpoch || s.Original != ScopesOriginal || s.View != ScopeView || s.Texture is not { Width: 768, Height: 256 } t || string.IsNullOrWhiteSpace(t.SharedHandleHex) ||
            !ulong.TryParse(t.SharedHandleHex.Replace("0x","",StringComparison.OrdinalIgnoreCase),NumberStyles.HexNumber,CultureInfo.InvariantCulture,out var handle) || handle == 0) {
            ScopeSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,$"grade:scopes:{InstanceId}","Scopes");
            ScopeStatus = s?.Status == "unavailable" ? "Native scopes unavailable; no measurement available." : "Preparing native scopes; no measurement available."; return;
        }
        ScopeStatus = $"{(ScopesOriginal ? "Original" : "Graded")} · {s.Status} · unchanged {s.SourceAgeMs/1000d:0.0}s · sample {s.SampleWidth}×{s.SampleHeight} · Rec.709 SDR assumed · histogram shared RGB peak · waveform 0–100% · log density · vectorscope 75% targets";
        ScopeSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,$"grade:scopes:{InstanceId}","Scopes") with {
            StatusLine = ScopeStatus, PendingSharedHandle = new SharedTextureHandle { NtHandle = handle, Width = t.Width,
                Height = t.Height, Format = t.Format, FrameNumber = t.FrameNumber } };
    }
    public bool HasAdvancedAdjustments => GradeDocument is { } doc && (doc.Bypass || doc.Intensity != 1 || doc.Operations.Length > 0);
    public string AdvancedIndicator => GradeDocument is not { } doc ? "No advanced adjustments" : doc.Bypass ? "Grade bypassed" :
        doc.Intensity == 0 ? "Overall grade intensity 0" : doc.Operations.Any(o=>o.Enabled && o.Intensity>0) ? "Advanced adjustments active" :
        doc.Operations.Length>0 ? "Advanced adjustments disabled" : doc.Intensity!=1 ? $"Overall grade intensity {doc.Intensity:P0}" : "No advanced adjustments";
    public AdvancedGradeDocument Document => GradeDocument?.Copy() ?? new();
    public event EventHandler? AdvancedDocumentChanged;
    private void InitializeAdvancedGrade(AdvancedGradeDocument? document)
    {
        GradeDocument = document?.Copy();
        foreach (var operation in document?.Operations ?? []) Adjustments.Add(operation.Copy());
        SelectedAdjustment = Adjustments.FirstOrDefault();
    }
    partial void OnAdvancedExpandedChanged(bool value)
    {
        if (value) { LiveEditing = false; EditStatus = "Draft — Apply Live sends this grade to the show."; }
        RequestPreview();
    }
    public void EditDocument(AdvancedGradeDocument document)
    {
        try { document.Validate(); }
        catch (ArgumentException ex) { EditStatus = ex.Message; return; }
        RememberGrade();
        SetDocument(document.Copy()); OnGradeEdited();
    }
    private void TrimHistory()
    {
        var retained = _undo.Take(100).Reverse().ToArray(); _undo.Clear();
        foreach (var document in retained) _undo.Push(document);
    }
    private void SetDocument(AdvancedGradeDocument? document)
    {
        var selectedId = SelectedAdjustment?.Id;
        GradeDocument = document;
        // Structural updates only; the native preview never rebuilds this list.
        var operations = document?.Operations ?? [];
        if (!Adjustments.Select(o=>o.Id).SequenceEqual(operations.Select(o=>o.Id))) {
            Adjustments.Clear(); foreach (var op in operations) Adjustments.Add(op.Copy());
        }
        SelectedAdjustment = operations.FirstOrDefault(o => o.Id == selectedId) ?? operations.FirstOrDefault();
        OnPropertyChanged(nameof(HasAdvancedAdjustments)); OnPropertyChanged(nameof(AdvancedIndicator));
        OnPropertyChanged(nameof(Document)); AdvancedDocumentChanged?.Invoke(this, EventArgs.Empty);
    }
    public void EditAdjustment(GradeOperation replacement)
    {
        var doc = Document;
        EditDocument(doc with { Operations = doc.Operations.Select(o => o.Id == replacement.Id ? replacement.Copy() : o).ToArray() });
    }
    public void SetCurve(int channel, IReadOnlyList<GradeCurvePoint> points)
    {
        if (channel is < 0 or > 3 || SelectedAdjustment is not { Kind: "curves" } selected) return;
        var curves = selected.Curves.Select(c => c.ToArray()).ToArray(); curves[channel] = points.ToArray();
        EditAdjustment(selected with { Curves = curves });
    }
    [RelayCommand] private void ExpandGrade() => AdvancedExpanded = !AdvancedExpanded;
    [RelayCommand] private void AddPrimaries() => AddAdjustment("primaries");
    [RelayCommand] private void AddCurves() => AddAdjustment("curves");
    private void AddAdjustment(string kind)
    {
        if (Adjustments.Count >= 8) { EditStatus = "A grade can contain up to eight adjustments."; return; }
        var added = new GradeOperation { Kind = kind }; EditDocument(Document with { Operations = [.. Document.Operations, added] });
        SelectedAdjustment = Adjustments.First(o => o.Id == added.Id);
    }
    [RelayCommand] private void RemoveAdjustment()
    {
        if (SelectedAdjustment is not { } selected) return;
        EditDocument(Document with { Operations = Document.Operations.Where(o => o.Id != selected.Id).ToArray() });
    }
    [RelayCommand] private void DuplicateAdjustment()
    {
        if (SelectedAdjustment is not { } selected || Adjustments.Count >= 8) return;
        var added = selected.Copy() with { Id = Guid.NewGuid().ToString("N") };
        EditDocument(Document with { Operations = [.. Document.Operations, added] });
        SelectedAdjustment = Adjustments.First(o => o.Id == added.Id);
    }
    [RelayCommand] private void MoveAdjustmentUp() => MoveAdjustment(-1);
    [RelayCommand] private void MoveAdjustmentDown() => MoveAdjustment(1);
    private void MoveAdjustment(int direction)
    {
        if (SelectedAdjustment is not { } selected) return;
        var operations = Document.Operations; var index = Array.FindIndex(operations, o => o.Id == selected.Id);
        var next = index + direction; if (index < 0 || next < 0 || next >= operations.Length) return;
        (operations[index], operations[next]) = (operations[next], operations[index]); EditDocument(Document with { Operations = operations });
    }
    [RelayCommand] private void UndoGrade()
    {
        if (!_undo.TryPop(out var previous)) return;
        _redo.Push(CopyGrade(CurrentGrade)); RestoreGrade(previous);
    }
    [RelayCommand] private void RedoGrade()
    {
        if (!_redo.TryPop(out var next)) return;
        _undo.Push(CopyGrade(CurrentGrade)); RestoreGrade(next);
    }
    [RelayCommand] private void ResetAdvanced() => EditDocument(new());
    [RelayCommand(CanExecute = nameof(CanSubmitGrade))] private async Task ApplyLive() => await SubmitGradeAsync(CurrentGrade);
    public string CopyGradeJson() => JsonSerializer.Serialize(new { presetVersion = 1, grade = CurrentGrade }, new JsonSerializerOptions(JsonSerializerDefaults.Web));
    public bool PasteGradeJson(string json)
    {
        try {
            if (json.Length > 16_500_000) throw new ArgumentException("Grade document is too large.");
            using var parsed = JsonDocument.Parse(json);
            if (parsed.RootElement.TryGetProperty("presetVersion",out var version)) {
                if (version.GetInt32()!=1 || !parsed.RootElement.TryGetProperty("grade",out var node)) throw new ArgumentException("Unsupported grade preset.");
                var grade = node.Deserialize<CoreVideoPro.WinUI.Models.ColorGrade>(new JsonSerializerOptions(JsonSerializerDefaults.Web))
                    ?? throw new ArgumentException("Missing grade.");
                if (!LutOptions.Contains(grade.Lut) || new[] { grade.Exposure,grade.Contrast,grade.Saturation,grade.Temperature }.Any(v=>v is < -100 or > 100))
                    throw new ArgumentException("Unsupported Basic grade.");
                grade.Advanced?.Validate();
                // Load is one draft action; Basic live editing cannot leak intermediate values.
                var live = LiveEditing; LiveEditing = false;
                RememberGrade(); RestoreGrade(grade);
                if (live) LiveEditing = true;
            } else {
                var document = JsonSerializer.Deserialize<AdvancedGradeDocument>(json, new JsonSerializerOptions(JsonSerializerDefaults.Web))
                    ?? throw new ArgumentException("Missing grade document.");
                document.Validate(); EditDocument(document);
            }
            return true;
        } catch (Exception ex) when (ex is JsonException or ArgumentException or InvalidOperationException or FormatException or OverflowException) { EditStatus = ex.Message; return false; }
    }
    public void ImportCube(string text, string name)
    {
        try {
            CubeLutParser.Parse(text);
            if (Adjustments.Count >= 8) throw new ArgumentException("A grade can contain up to eight adjustments.");
            var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(text))).ToLowerInvariant();
            var op = new GradeOperation { Kind = "cube", Name = name, CubeText = text, CubeSha256 = hash };
            EditDocument(Document with { Operations = [.. Document.Operations,op] }); SelectedAdjustment = op;
        } catch (ArgumentException ex) { EditStatus = ex.Message; }
    }
}
