using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private int _programBufferFrames = ProgramBufferPreference.DefaultFrames;
    private int _startupProgramBufferFrames = ProgramBufferPreference.DefaultFrames;

    public IReadOnlyList<int> ProgramBufferFrameOptions { get; } = [2, 3];
    public int ProgramBufferSessionRequestedFrames => _startupProgramBufferFrames;

    public bool ProgramBufferRestartRequired => ProgramBufferSettingsSummary.RequiresRestart(_startupProgramBufferFrames, ProgramBufferFrames);
    public string ProgramBufferSessionSummary => ProgramBufferSettingsSummary.Describe(_startupProgramBufferFrames, ProgramBufferFrames);

    public int ProgramBufferFrames
    {
        get => _programBufferFrames;
        set
        {
            var frames = ProgramBufferPreference.Normalize(value);
            if (!_outputPreferencesLoaded) { CommitProgramBufferSelection(frames); return; }
            try { SaveProgramBufferFrames(frames); }
            catch (Exception ex)
            {
                CommandStatus = "Program buffer setting was not saved: " + ex.Message;
                // Re-read the retained value after a failed TwoWay ComboBox write.
                OnPropertyChanged(nameof(ProgramBufferFrames));
            }
        }
    }

    public void SaveProgramBufferFrames(int frames)
    {
        if (!_outputPreferencesLoaded) throw new InvalidOperationException("Production preferences are still loading.");
        try
        {
            ProgramBufferPreferencePersistence.Save(_outputPreferencesStore, CaptureProductionOutputPreferences(),
                frames, CommitProgramBufferSelection);
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("prefs: program buffer save failed", ex);
            throw;
        }
    }

    private void CommitProgramBufferSelection(int frames)
    {
        if (!SetProperty(ref _programBufferFrames, frames, nameof(ProgramBufferFrames))) return;
        OnPropertyChanged(nameof(ProgramBufferRestartRequired));
        OnPropertyChanged(nameof(ProgramBufferSessionSummary));
    }
}
