using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

// One diagnostic surface reused by Settings and the existing detached window.
public sealed partial class DiagnosticsView : UserControl
{
    public DiagnosticsView() => InitializeComponent();
    public StudioViewModel? ViewModel
    {
        get => (StudioViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }
    public static readonly DependencyProperty ViewModelProperty = DependencyProperty.Register(
        nameof(ViewModel), typeof(StudioViewModel), typeof(DiagnosticsView), new PropertyMetadata(null));
}
