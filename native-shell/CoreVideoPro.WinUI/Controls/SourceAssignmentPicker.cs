using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>Explicit source patching; opening or hydrating the picker never assigns.</summary>
internal sealed class SourceAssignmentPicker : DropDownButton
{
    public ShowInputSlotViewModel? Input
    {
        get => (ShowInputSlotViewModel?)GetValue(InputProperty);
        set => SetValue(InputProperty, value);
    }
    public static readonly DependencyProperty InputProperty = DependencyProperty.Register(
        nameof(Input), typeof(ShowInputSlotViewModel), typeof(SourceAssignmentPicker), new PropertyMetadata(null, (sender, _) => ((SourceAssignmentPicker)sender).BindInput()));
    private bool _retired;
    private readonly bool _compact;
    private readonly TextBlock _label = SourcesTableRowControl.Text("");
    internal MenuFlyout SourceMenu { get; } = new();
    internal string DisplayLabel => _label.Text;

    internal SourceAssignmentPicker(bool compact = false)
    {
        DefaultStyleKey = typeof(DropDownButton);
        HorizontalContentAlignment = HorizontalAlignment.Left;
        _compact = compact;
        MinWidth = 0; FontSize = 12; Padding = new(6, 4, 6, 4);
        HorizontalAlignment = HorizontalAlignment.Stretch;
        Background = SourcesTableRowControl.Brush(24, 39, 48);
        Foreground = SourcesTableRowControl.Brush(231, 240, 243);
        var content = new Grid { ColumnSpacing = 8, HorizontalAlignment = HorizontalAlignment.Stretch };
        content.ColumnDefinitions.Add(new() { Width = new(1, GridUnitType.Star) });
        content.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        content.Children.Add(_label);
        var arrow = SourcesTableRowControl.Text("▾"); Grid.SetColumn(arrow, 1); content.Children.Add(arrow);
        Content = content; IsEnabled = false;
        Flyout = SourceMenu;
        SourceMenu.Opening += (_, _) => PrepareMenu();
    }

    private void BindInput()
    {
        if (_retired || Input is not { } input)
        {
            _label.ClearValue(TextBlock.TextProperty); _label.Text = "";
            ClearValue(IsEnabledProperty); IsEnabled = false;
            return;
        }
        SourcesTableRowControl.Bind(_label, TextBlock.TextProperty, input,
            _compact ? "OverviewName" : "SelectedUnifiedSourceLabel");
        // This picker chooses the kind as well as the source. The legacy per-kind
        // gate disables Unassigned slots and strands their only assignment control.
        ClearValue(IsEnabledProperty); IsEnabled = true;
    }

    // Build only on explicit opening. Roster updates cannot replace an open popup.
    internal void PrepareMenu()
    {
        SourceMenu.Items.Clear();
        if (_retired || Input is not { } input) return;
        var identity = input.SourceId;
        foreach (var group in ShowInputRosterService.UnifiedSourceGroups)
        {
            var options = input.UnifiedSourceOptions.Where(option => option.Group == group).ToArray();
            if (options.Length == 0) continue;
            var category = new MenuFlyoutSubItem { Text = group };
            foreach (var option in options)
            {
                var item = new MenuFlyoutItem
                {
                    Text = option.Label,
                    IsEnabled = !ShowInputRosterService.IsHintSourceId(option.Value)
                };
                item.Click += (_, _) =>
                {
                    if (!_retired && ReferenceEquals(Input, input) && input.SourceId == identity &&
                        input.UnifiedSourceOptions.Any(current => current.Value == option.Value))
                        input.SelectedUnifiedSourceId = option.Value;
                };
                category.Items.Add(item);
            }
            SourceMenu.Items.Add(category);
        }
    }
    internal void Retire() { _retired = true; SourceMenu.Hide(); Input = null; }
}
