using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Views;

internal static class SourceLowerThirdFlyout
{
    internal static Flyout Create(Button owner, ShowInputSlotViewModel editor)
    {
        var sourceId = editor.SourceId;
        var name = new TextBox { Header = "Name", Text = editor.DisplayName };
        var secondary = new TextBox { Header = "Title, organization or website", Text = editor.SecondaryLine };
        var mode = new ComboBox { Header = "Secondary line", ItemsSource = new[] { "Source default", "Custom text", "Hide secondary line" },
            SelectedIndex = (int)SourceLowerThirdText.Mode(editor.UsesDefaultSecondaryLine, editor.SecondaryLine), HorizontalAlignment = HorizontalAlignment.Stretch };
        var reset = new Button { Content = "Reset name to source default" };
        var previewName = new TextBlock { FontSize = 18, TextWrapping = TextWrapping.Wrap };
        var previewSecondary = new TextBlock { FontSize = 13, TextWrapping = TextWrapping.Wrap };
        var preview = new StackPanel { Spacing = 4 };
        preview.Children.Add(previewName); preview.Children.Add(previewSecondary);
        var notice = new TextBlock { Text = "Apply saves text for this source and updates a lower third already following it. Its appearance comes from the look assigned in Graphics.", TextWrapping = TextWrapping.Wrap, FontSize = 12 };
        var apply = new Button { Content = "Apply guest text", HorizontalAlignment = HorizontalAlignment.Right };
        var panel = new StackPanel { Width = 350, Spacing = 10 };
        panel.Children.Add(new TextBlock { Text = "Lower-third text", FontSize = 18 });
        panel.Children.Add(name); panel.Children.Add(reset); panel.Children.Add(mode);
        panel.Children.Add(secondary); panel.Children.Add(new TextBlock { Text = "Text preview", FontSize = 12 });
        panel.Children.Add(new Border { Padding = new Thickness(12), Background = new SolidColorBrush(Microsoft.UI.Colors.DarkSlateGray), Child = preview });
        panel.Children.Add(notice); panel.Children.Add(apply);
        var flyout = new Flyout { Content = panel };
        void UpdatePreview()
        {
            secondary.Visibility = mode.SelectedIndex == (int)SecondaryLineMode.Custom ? Visibility.Visible : Visibility.Collapsed;
            previewName.Text = string.IsNullOrWhiteSpace(name.Text) ? editor.DefaultDisplayName : name.Text.Trim();
            var value = SourceLowerThirdText.Value((SecondaryLineMode)mode.SelectedIndex, secondary.Text);
            previewSecondary.Text = value ?? editor.DefaultSecondaryLine;
            previewSecondary.Visibility = previewSecondary.Text.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        }
        name.TextChanged += (_, _) => UpdatePreview();
        secondary.TextChanged += (_, _) => UpdatePreview();
        mode.SelectionChanged += (_, _) => UpdatePreview();
        reset.Click += (_, _) => name.Text = editor.DefaultDisplayName;
        apply.Click += (_, _) =>
        {
            if (!ReferenceEquals(owner.Tag, editor) || sourceId is null || editor.SourceId != sourceId)
            {
                apply.IsEnabled = false;
                notice.Text = "Source assignment changed. Reopen Text for the current source.";
                return;
            }
            var value = SourceLowerThirdText.Value((SecondaryLineMode)mode.SelectedIndex, secondary.Text);
            editor.ApplyLowerThirdText(name.Text, value ?? "", value is null);
            flyout.Hide();
        };
        UpdatePreview();
        return flyout;
    }
}
