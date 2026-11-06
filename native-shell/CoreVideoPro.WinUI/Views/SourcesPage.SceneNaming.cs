using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage
{
    private async void OnCreateNamedScene(object sender, RoutedEventArgs e)
    {
        if (ViewModel is { } studio && XamlRoot is { } root)
            await SceneNamingDialog.CreateAsync(studio, root, (sender as FrameworkElement)?.Tag as string);
    }

    private async void OnRenameScene(object sender, RoutedEventArgs e)
    {
        if (ViewModel is { } studio && XamlRoot is { } root)
            await SceneNamingDialog.RenameAsync(studio, root,
                (sender as FrameworkElement)?.Tag as string ?? studio.PreviewSceneId);
    }
}
