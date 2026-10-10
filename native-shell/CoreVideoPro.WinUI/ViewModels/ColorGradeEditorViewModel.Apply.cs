using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class ColorGradeEditorViewModel
{
    public Func<ColorGrade,ulong,long,Task<SourceGradeApplyOutcome>>? ApplyGradeAsync { get; set; }
    [ObservableProperty, NotifyPropertyChangedFor(nameof(CanEditControls)), NotifyCanExecuteChangedFor(nameof(ApplyLiveCommand)), NotifyCanExecuteChangedFor(nameof(SaveCommand))]
    private bool _isApplying;
    public bool CanEditControls => !IsApplying;
    public long AppliedRevision => _appliedRevision;
    public event EventHandler? GradeAuthorityReset;
    private long _authorityGeneration;
    private bool _hasAuthority, _identityChanged, _closed;
    private ulong _sourceEpoch;
    private long _appliedRevision;
    private ColorGrade? _pendingLiveGrade;
    private bool CanSubmitGrade() => !IsApplying && !_identityChanged && !_closed;
    public void ResetGradeAuthority() { ++_authorityGeneration; _hasAuthority = false; _identityChanged = false; _appliedRevision = 0; _pendingLiveGrade = null;
        GradeAuthorityReset?.Invoke(this,EventArgs.Empty);SaveCommand.NotifyCanExecuteChanged();ApplyLiveCommand.NotifyCanExecuteChanged(); }
    private void ObserveGradeAuthority(GradePreviewObservation observation)
    {
        if (_hasAuthority && observation.SourceEpoch != _sourceEpoch) {
            _identityChanged = true; EditStatus = "Source identity changed. Close and reopen its grade editor before applying.";
            SaveCommand.NotifyCanExecuteChanged(); ApplyLiveCommand.NotifyCanExecuteChanged(); return;
        }
        if (!_hasAuthority) { _hasAuthority = true; _sourceEpoch = observation.SourceEpoch; }
        if (observation.AppliedRevision != _appliedRevision) EditStatus = "Live grade changed since this draft opened. Close and reopen to review the current grade; an old revision cannot overwrite it.";
        if (_pendingLiveGrade is not null && !IsApplying) { var grade = _pendingLiveGrade; _pendingLiveGrade = null; _ = SubmitGradeAsync(grade); }
    }
    private void QueueLiveGrade()
    {
        if (ApplyGradeAsync is null) { GradeChanged?.Invoke(this,CurrentGrade); return; }
        _pendingLiveGrade = CurrentGrade;
        if (!_hasAuthority || IsApplying || _closed) return;
        var grade = _pendingLiveGrade; _pendingLiveGrade = null; _ = SubmitGradeAsync(grade);
    }
    private async Task<bool> SubmitGradeAsync(ColorGrade grade)
    {
        if (ApplyGradeAsync is null) return true; // Existing design/test hosts have no native apply owner.
        if (!CanSubmitGrade()) return false;
        if (!_hasAuthority) { EditStatus = "Wait for an identified native source preview before applying."; return false; }
        var generation = _authorityGeneration; var epoch = _sourceEpoch;
        IsApplying = true; EditStatus = "Applying grade…";
        try {
            var outcome = await ApplyGradeAsync(grade,_sourceEpoch,_appliedRevision);
            if (generation != _authorityGeneration || _identityChanged || (outcome.Accepted && outcome.SourceEpoch != epoch)) { _pendingLiveGrade = null; EditStatus = "Source changed during apply. Grade acknowledgement is no longer current; review the draft again."; return false; }
            if (!outcome.Accepted) { _pendingLiveGrade = null; EditStatus = $"Grade was not applied: {outcome.Reason}. Previous live grade retained."; return false; }
            _appliedRevision = outcome.Revision;
            EditStatus = $"Live grade acknowledged · revision {_appliedRevision}";
            if (!_closed) GradeChanged?.Invoke(this,grade);
            return true;
        } catch (Exception ex) { _pendingLiveGrade = null; EditStatus = $"Grade was not applied: {ex.Message}"; return false; }
        finally {
            IsApplying = false;
            if (_pendingLiveGrade is not null && LiveEditing && !_closed) { var next = _pendingLiveGrade; _pendingLiveGrade = null; _ = SubmitGradeAsync(next); }
        }
    }
    public void StopGradeEditing() { _closed = true; _pendingLiveGrade = null; }
}
