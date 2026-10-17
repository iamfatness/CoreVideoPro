using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
namespace CoreVideoPro.WinUI.ViewModels;
public sealed partial class StudioViewModel
{
    [RelayCommand]
    private async Task ToggleProgramLowerThirdAsync()
    {
        var visible = ManagedLowerThirdAuthority ? ManagedLowerThirdVisible : ProgramLowerThirdKey.IsVisible || ProgramLowerThirdEnabled;
        await ApplyStudioLowerThirdIntentAsync(!visible);
    }

    private void ToggleLegacyProgramLowerThird()
    {
        if (ProgramLowerThirdKey.IsVisible || ProgramLowerThirdEnabled)
        {
            ProgramLowerThirdEnabled = false;
            _programLowerThirdAutomationSuppressed = true;
            foreach (var graphic in Graphics.Where(graphic =>
                graphic.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase)))
            {
                graphic.Enabled = false;
            }

            CommandStatus = "Lower third keyed out";
        }
        else
        {
            if (ResolveProgramLowerThirdSource(ProgramSceneRoutes) is null)
            {
                CommandStatus = "Lower third needs a program source";
                OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
                OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
                OnPropertyChanged(nameof(StudioLowerThirdToolTip));
                return;
            }

            _programLowerThirdAutomationSuppressed = false;
            ProgramLowerThirdEnabled = true;
            CommandStatus = "Lower third keyed in";
        }

        OnPropertyChanged(nameof(EnabledGraphics));
        RefreshProgramLowerThirdKeyPosition();
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void RebuildProgramLowerThird()
    {
        if(ManagedLowerThirdAuthority) {_ = ApplyManagedLowerThirdIntentAsync(true,true);return;}
        if (ResolveProgramLowerThirdSource(ProgramSceneRoutes) is not { } source)
        {
            CommandStatus = "Lower third needs a program source";
            OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
            OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
            OnPropertyChanged(nameof(StudioLowerThirdToolTip));
            return;
        }

        _programLowerThirdAutomationSuppressed = false;
        ProgramLowerThirdEnabled = true;
        UpdateProgramLowerThirdKey(source, force: true);
        CommandStatus = "Lower third rebuilt";
        _ = TrySyncMediaCoreAsync();
    }

}
