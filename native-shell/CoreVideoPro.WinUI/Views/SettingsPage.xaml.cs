using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SettingsPage : UserControl
{
    public SettingsPage()
    {
        InitializeComponent();
    }

    public SettingsViewModel? ViewModel
    {
        get => (SettingsViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(
            nameof(ViewModel),
            typeof(SettingsViewModel),
            typeof(SettingsPage),
            new PropertyMetadata(null));

    private void RecentMeetings_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ViewModel is null || sender is not ComboBox comboBox)
        {
            return;
        }

        if (comboBox.SelectedItem is RecentZoomMeeting meeting)
        {
            ViewModel.UseRecentMeetingCommand.Execute(meeting);
            comboBox.SelectedItem = null;
        }
    }
}
