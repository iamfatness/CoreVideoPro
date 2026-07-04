using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// The audio crosspoint matrix + selected-crosspoint gain editor, shared by
/// the Routing tab and the Audio tab (redesign B4). Both hosts bind the SAME
/// StudioViewModel, so the single AudioRoutingMatrixViewModel instance keeps
/// selection, gain edits, and core snapshot hydration consistent everywhere.
/// </summary>
public sealed partial class AudioRoutingMatrixPanel : UserControl
{
    public AudioRoutingMatrixPanel()
    {
        InitializeComponent();
    }

    public StudioViewModel? ViewModel
    {
        get => (StudioViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(
            nameof(ViewModel),
            typeof(StudioViewModel),
            typeof(AudioRoutingMatrixPanel),
            new PropertyMetadata(null, static (d, _) => ((AudioRoutingMatrixPanel)d).Bindings.Update()));
}
