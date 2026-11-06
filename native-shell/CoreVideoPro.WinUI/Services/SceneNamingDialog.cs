using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Services;

internal static class SceneNamingDialog
{
    internal static Task CreateAsync(StudioViewModel studio, XamlRoot root, string? template) =>
        ShowAsync(studio, root, null, template, studio.SuggestedSceneName(template));

    internal static Task RenameAsync(StudioViewModel studio, XamlRoot root, string sceneId)
    {
        var scene = studio.Scenes.FirstOrDefault(scene => scene.Id == sceneId);
        return scene is null ? Task.CompletedTask : ShowAsync(studio, root, sceneId, null, scene.Name);
    }

    private static async Task ShowAsync(StudioViewModel studio, XamlRoot root, string? sceneId,
        string? template, string initialName)
    {
        var field = new TextBox { Name = "SceneNameField", Header = "Scene name", Text = initialName,
            MinWidth = 240, PlaceholderText = "Enter a scene name" };
        AutomationProperties.SetName(field, "Scene name");
        var error = new TextBlock { Name = "SceneNameError", TextWrapping = TextWrapping.Wrap };
        var content = new StackPanel { Spacing = 8 };
        content.Children.Add(field);
        content.Children.Add(error);
        var dialog = new ContentDialog { XamlRoot = root, Title = sceneId is null ? "Name new scene" : "Rename scene",
            Content = content, PrimaryButtonText = sceneId is null ? "Create scene" : "Rename",
            CloseButtonText = "Cancel", DefaultButton = ContentDialogButton.Primary };
        string? validatedText = null;
        // TextChanged can be delivered after a fast primary-button invocation.
        // An event for the submitted value must not erase its validation error.
        field.TextChanged += (_, _) => { if (field.Text != validatedText) error.Text = ""; };
        dialog.Opened += (_, _) => { field.Focus(FocusState.Programmatic); field.SelectAll(); };
        dialog.PrimaryButtonClick += (_, args) =>
        {
            // Capture identity when opening; selection changes cannot redirect a rename.
            string? reason;
            validatedText = field.Text;
            var accepted = sceneId is null
                ? studio.TryCreateNamedScene(template, field.Text, out reason)
                : studio.TryRenameScene(sceneId, field.Text, out reason);
            if (!accepted) { args.Cancel = true; error.Text = reason ?? "Unable to save this scene name."; }
        };
        await dialog.ShowAsync();
    }
}
