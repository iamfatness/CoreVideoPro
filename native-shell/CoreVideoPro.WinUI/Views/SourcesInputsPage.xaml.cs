using System;
using System.Collections.Generic;
using System.Linq;
using CoreVideoPro.WinUI;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
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

        // Resolve the slot view-model by index (DataContext is null for elements realized
        // inside an ItemsRepeater). We set each ComboBox's SelectedValue explicitly here —
        // after the element + its ItemsSource are realized — because the OneWay x:Bind
        // selection does not reliably resolve at realization for the Kind combo (its bound
        // value never gets a post-realization PropertyChanged the way Source does), leaving
        // the TYPE column blank. Setting it with the SelectionChanged handler detached keeps
        // the model authoritative without a spurious write-back.
        // SRC-1: categorized source picker — one submenu per group so long device lists
        // stay filterable at pick time. The menu is (re)built on every open from the
        // button's CURRENT Tag (x:Bind editor), so ItemsRepeater recycling stays correct.
        var unifiedButton = FindDescendant<DropDownButton>(root, "UnifiedSourceButton");
        if (unifiedButton is null)
        {
            return;
        }

        if (unifiedButton.Flyout is not MenuFlyout)
        {
            var flyout = new MenuFlyout
            {
                Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedLeft
            };
            flyout.Opening += (_, _) => BuildUnifiedSourceMenu(flyout, unifiedButton);
            unifiedButton.Flyout = flyout;
        }

        var inputMicCombo = FindDescendant<ComboBox>(root, "InputMicCombo");
        if (inputMicCombo is not null)
        {
            inputMicCombo.SelectionChanged -= OnShowInputMicChanged;
            inputMicCombo.SelectionChanged += OnShowInputMicChanged;
        }

    }

    // Rebuilds the categorized picker menu from the button's current editor. Groups render
    // in fixed order (Zoom / Camera / Screen / Media / SRT); empty groups with a hint row
    // show the hint disabled ("No media assets — add them on the Media tab"); groups with
    // nothing at all are omitted.
    private static void BuildUnifiedSourceMenu(MenuFlyout flyout, DropDownButton button)
    {
        if (button.Tag is not ShowInputSlotViewModel editor)
        {
            return;
        }

        flyout.Items.Clear();
        foreach (var group in ShowInputRosterService.UnifiedSourceGroups)
        {
            var entries = editor.UnifiedSourceOptions
                .Where(option => string.Equals(option.Group, group, System.StringComparison.Ordinal))
                .ToList();
            if (entries.Count == 0)
            {
                continue;
            }

            var subMenu = new MenuFlyoutSubItem { Text = group };
            foreach (var entry in entries)
            {
                if (ShowInputRosterService.IsHintSourceId(entry.Value))
                {
                    subMenu.Items.Add(new MenuFlyoutItem { Text = entry.Label, IsEnabled = false });
                    continue;
                }

                var value = entry.Value;
                var item = new MenuFlyoutItem { Text = entry.Label };
                item.Click += (_, _) =>
                {
                    // Read the Tag at CLICK time — recycling may have rebound the row.
                    if (button.Tag is ShowInputSlotViewModel current)
                    {
                        current.SelectedUnifiedSourceId = value;
                        LaunchLog.Write(
                            $"sources: source selected '{value}' slot={current.SlotNumber} -> kind={current.Kind}");
                    }
                };
                subMenu.Items.Add(item);
            }

            flyout.Items.Add(subMenu);
        }
    }

    private void OnSourceOptionsClick(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: ShowInputSlotViewModel editor } button ||
            !editor.ShowSourcePolicies || ViewModel is not { } studio) return;
        try
        {
            var row = studio.FeedHealthRows.FirstOrDefault(item =>
                string.Equals(item.ParticipantId, editor.ParticipantId, StringComparison.Ordinal));
            var flyout = SourcePolicyFlyout.Create(button, editor,
                studio.ProductionRoleAssignmentOptions, studio.DropoutPolicyOptions,
                row?.ProductionRoleId ?? string.Empty,
                studio.SourceDropoutPolicy("zoom:" + editor.ParticipantId), row is not null,
                studio.SetParticipantProductionRole,
                (participantId, policy) => studio.SetSourceDropoutPolicy("zoom:" + participantId, policy));
            flyout.ShowAt(button);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"sources: options could not open ({ex.GetType().Name}: {ex.Message})");
        }
    }
    private void OnShowInputMicChanged(object sender, SelectionChangedEventArgs e)
    {
        // Use Tag (x:Bind to the slot view-model), not DataContext, which is null for
        // ComboBoxes realized inside the ItemsRepeater.
        if (sender is not ComboBox combo ||
            combo.Tag is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not string audioDeviceId)
        {
            return;
        }

        editor.AudioDeviceId = audioDeviceId;
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
