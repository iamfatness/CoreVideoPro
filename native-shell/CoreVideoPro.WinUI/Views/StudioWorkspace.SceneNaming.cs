using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class StudioWorkspace
{
    private void OnOpenHealth(object sender, RoutedEventArgs e)
    {
        ViewModel?.SelectTabCommand.Execute("settings");
        StudioSettingsPage.OpenHealth();
    }

    private void OnEditTiles(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string id }) ViewModel?.EditTiles(id);
    }

    private async void OnRenameScene(object sender, RoutedEventArgs e)
    {
        if (ViewModel is { } studio && XamlRoot is { } root && sender is FrameworkElement { Tag: string id })
            await SceneNamingDialog.RenameAsync(studio, root, id);
    }
}
