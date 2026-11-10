using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ProgramBlackoutBrush))]
    private bool _programBlackout;
    public Microsoft.UI.Xaml.Media.SolidColorBrush ProgramBlackoutBrush => new(ProgramBlackout
        ? Windows.UI.Color.FromArgb(255, 240, 82, 82) : Windows.UI.Color.FromArgb(255, 65, 72, 78));
    [ObservableProperty] private string _programBlackoutLabel = "Fade to Black";
    private bool _blackoutInFlight;

    private void ApplyProgramBlackoutSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        ProgramBlackout = snapshot.ProgramBlackout == true;
        ProgramBlackoutLabel = snapshot.ProgramBlackout switch
        {
            true when snapshot.ProgramBlackoutOpacity < .999 => "Fading to black",
            true => "BLACK · Restore",
            false when snapshot.ProgramBlackoutOpacity > .001 => "Restoring picture",
            _ => "Fade to Black"
        };
    }

    [RelayCommand]
    private async Task ToggleProgramBlackout()
    {
        var result = await SetProgramBlackoutAsync(null);
        if (!result.Ok) CommandStatus = result.Error ?? "FTB unavailable";
    }

    // All callers share admission and native observation, including remote set/toggle.
    public async Task<ControlInvokeResult> SetProgramBlackoutAsync(bool? enabled)
    {
        if (_blackoutInFlight) return ControlInvokeResult.Fail("FTB command is already in progress.");
        if (!_bridge.Running) return ControlInvokeResult.Fail("FTB unavailable: media core is offline.");
        _blackoutInFlight = true;
        try
        {
            var before = await _bridge.PollSnapshotAsync();
            if (before.ProgramBlackout is not bool current)
                return ControlInvokeResult.Fail("This media core does not support Fade to Black.");
            var target = enabled ?? !current;
            var snapshot = await _bridge.SyncAsync([new NativeMediaCoreCommand
            {
                Type = "program-blackout",
                ExtensionData = new()
                {
                    ["enabled"] = JsonSerializer.SerializeToElement(target),
                    ["durationMs"] = JsonSerializer.SerializeToElement(300)
                }
            }]);
            ApplyProgramBlackoutSnapshot(snapshot);
            if (snapshot.ProgramBlackout != target)
                return ControlInvokeResult.Fail("Media core did not acknowledge FTB.");
            CommandStatus = target ? "Fading Program to black — audio continues" : "Restoring Program picture";
            return ControlInvokeResult.Success;
        }
        catch (Exception ex)
        {
            CommandStatus = $"FTB unavailable: {ex.Message}";
            return ControlInvokeResult.Fail(CommandStatus);
        }
        finally { _blackoutInFlight = false; }
    }
}
