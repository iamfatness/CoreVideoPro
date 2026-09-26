using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    public NativeMediaCoreAudioMixSession? AppliedAudioMonitorSession => _bridge.LastSnapshot?.AudioMixSession;
    private bool _suppressAudioMonitorControlSubmission;
    private string _audioMonitorControlNotice = string.Empty;
    private MediaCoreAudioMonitorWire? _audioMonitorPendingDraft;
    private long _audioMonitorRequestVersion;
    private CancellationTokenSource? _audioMonitorEditDebounce;

    private async Task QueueAudioMonitorDraftAsync()
    {
        _audioMonitorEditDebounce?.Cancel();
        var debounce = new CancellationTokenSource();
        _audioMonitorEditDebounce = debounce;
        var draft = CurrentAudioMonitorDraft();
        _audioMonitorPendingDraft = draft;
        _audioMonitorControlNotice = "Monitor edit pending core application";
        OnPropertyChanged(nameof(AudioMonitorStatus));
        try
        {
            await Task.Delay(75, debounce.Token).ConfigureAwait(true);
            await SubmitAudioMonitorDraftAsync(draft).ConfigureAwait(true);
        }
        catch (OperationCanceledException) when (debounce.IsCancellationRequested) { }
        finally
        {
            if (ReferenceEquals(_audioMonitorEditDebounce, debounce)) _audioMonitorEditDebounce = null;
            debounce.Dispose();
        }
    }

    private MediaCoreAudioMonitorWire CurrentAudioMonitorDraft() => new(
        AudioMonitoringEnabled, SelectedAudioMonitorNativeDeviceId,
        SelectedAudioMonitorDeviceName, AudioMonitorVolume);

    private async Task<AudioMonitorControlOutcome> SubmitAudioMonitorDraftAsync(
        MediaCoreAudioMonitorWire draft, string? expectedEpoch = null,
        long? expectedRevision = null, string? operationId = null)
    {
        var requestVersion = Interlocked.Increment(ref _audioMonitorRequestVersion);
        _audioMonitorPendingDraft = draft;
        _audioMonitorControlNotice = "Monitor edit pending core application";
        OnPropertyChanged(nameof(AudioMonitorStatus));
        AudioMonitorControlOutcome outcome;
        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(true);
            outcome = await _bridge.SetAudioMonitorControlAsync(
                draft, expectedEpoch, expectedRevision, operationId).ConfigureAwait(true);
        }
        catch (Exception)
        {
            outcome = new(AudioMonitorControlOutcomeKind.Reconciling,
                operationId ?? string.Empty, draft, null, null);
        }

        if (requestVersion != Volatile.Read(ref _audioMonitorRequestVersion)) return outcome;
        _audioMonitorControlNotice = outcome.Kind switch
        {
            AudioMonitorControlOutcomeKind.Applied => string.Empty,
            AudioMonitorControlOutcomeKind.Conflict =>
                $"Monitor edit conflicted with core revision {outcome.Control?.Revision}; draft retained",
            AudioMonitorControlOutcomeKind.Incompatible => "Monitor control contract unavailable",
            _ => "Monitor edit reconciling with core"
        };
        if (outcome.Kind == AudioMonitorControlOutcomeKind.Applied)
        {
            _audioMonitorPendingDraft = null;
            SaveProductionOutputPreferences();
        }
        OnPropertyChanged(nameof(AudioMonitorStatus));
        return outcome;
    }

    /// <summary>Control API and local property edits share native admission.</summary>
    public async Task<AudioMonitorControlOutcome> RequestAudioMonitorControlAsync(
        bool? enabled = null, double? volume = null,
        string? expectedEpoch = null, long? expectedRevision = null,
        string? operationId = null)
    {
        var draft = CurrentAudioMonitorDraft() with
        {
            Enabled = enabled ?? AudioMonitoringEnabled,
            Volume = volume is null ? AudioMonitorVolume : NormalizeAudioMonitorVolume(volume.Value)
        };
        if (draft.Enabled && draft.Volume <= 0) draft = draft with { Volume = DefaultAudioMonitorVolume };
        var outcome = await SubmitAudioMonitorDraftAsync(draft, expectedEpoch, expectedRevision, operationId)
            .ConfigureAwait(true);
        if (outcome.Kind == AudioMonitorControlOutcomeKind.Applied)
        {
            _suppressAudioMonitorControlSubmission = true;
            try
            {
                AudioMonitoringEnabled = draft.Enabled;
                AudioMonitorVolume = draft.Volume;
            }
            finally
            {
                _suppressAudioMonitorControlSubmission = false;
            }
            SaveProductionOutputPreferences();
            RefreshAudioMonitorBindings();
        }
        return outcome;
    }
}
