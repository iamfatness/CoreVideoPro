using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace CoreVideoPro.WinUI.Views;

/// <summary>The NDI output controls shared by the Studio footer tile and Settings › NDI. The
/// on/off control is never two-way bound: it asks <see cref="StudioViewModel.SetNdiOutput"/> and
/// then re-reads the ViewModel, so a refusal visibly leaves it off (the status label carries the
/// reason). The NDI text boxes commit on LostFocus (x:Bind) or Enter (here), never per keystroke:
/// every NDI edit is validated, and a half-typed name could otherwise switch NDI off.</summary>
internal static class NdiOutputControls
{
    internal static void ToggleFromTile(StudioViewModel? viewModel, object sender)
    {
        if (viewModel is null || sender is not ToggleButton tile) return;
        viewModel.SetNdiOutput(!viewModel.NdiOutputEnabled);
        tile.IsChecked = viewModel.NdiOutputEnabled;
    }

    internal static void ToggleFromSwitch(StudioViewModel? viewModel, object sender)
    {
        // x:Bind setting IsOn from the ViewModel raises Toggled too; that is not a request.
        if (viewModel is null || sender is not ToggleSwitch toggle || toggle.IsOn == viewModel.NdiOutputEnabled) return;
        viewModel.SetNdiOutput(!viewModel.NdiOutputEnabled);
        toggle.IsOn = viewModel.NdiOutputEnabled;
    }

    /// <summary>Enter commits the box like LostFocus. The box's Tag names the field.</summary>
    internal static void CommitTextOnEnter(StudioViewModel? viewModel, object sender, KeyRoutedEventArgs e)
    {
        if (e.Key != VirtualKey.Enter || viewModel is null || sender is not TextBox { Tag: string field } box) return;
        switch (field)
        {
            case "ProgramName":
                viewModel.StreamNdiProgramName = box.Text;
                break;
            case "GroupName":
                viewModel.StreamNdiGroupName = box.Text;
                break;
            case "GraphicsName":
                viewModel.StreamNdiGraphicsName = box.Text;
                break;
            default:
                return;
        }

        e.Handled = true;
    }
}
