using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesInputsPage : UserControl
{
    public SourcesInputsPage()
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
            typeof(SourcesInputsPage),
            new PropertyMetadata(null));

    private void OnShowInputEditorPrepared(ItemsRepeater sender, ItemsRepeaterElementPreparedEventArgs args)
    {
        if (args.Element is not FrameworkElement root)
        {
            return;
        }

        var kindCombo = FindDescendant<ComboBox>(root, "KindCombo");
        if (kindCombo is null)
        {
            return;
        }

        kindCombo.SelectionChanged -= OnShowInputKindChanged;
        kindCombo.SelectionChanged += OnShowInputKindChanged;

        var sourceCombo = FindDescendant<ComboBox>(root, "SourceCombo");
        if (sourceCombo is null)
        {
            return;
        }

        sourceCombo.SelectionChanged -= OnShowInputSourceChanged;
        sourceCombo.SelectionChanged += OnShowInputSourceChanged;
    }

    private void OnShowInputKindChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.DataContext is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not ShowInputKind kind)
        {
            return;
        }

        editor.Kind = kind;
    }

    private void OnShowInputSourceChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.DataContext is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not string sourceId)
        {
            return;
        }

        editor.SelectedSourceId = sourceId;
    }

    private static T? FindDescendant<T>(DependencyObject root, string name) where T : FrameworkElement
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var index = 0; index < count; index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T match && match.Name == name)
            {
                return match;
            }

            var descendant = FindDescendant<T>(child, name);
            if (descendant is not null)
            {
                return descendant;
            }
        }

        return null;
    }
}
