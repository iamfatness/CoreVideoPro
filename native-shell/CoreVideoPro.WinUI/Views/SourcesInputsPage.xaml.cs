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

    // Apply the row's current role once the container exists, instead of letting
    // x:Bind push it during ProcessBindings.
    //
    // THIS CRASHED THE APP (2026-08-09, twice in ten minutes). x:Bind drove
    // Selector.SelectedValue from a phased binding update and it threw
    // COMException 0x80004005 straight out of set_SelectedValue — unhandled, so
    // the process died; the dumps bucket as
    // STOWED_EXCEPTION_80004003_CoreMessagingXP.dll!DispatcherQueue::DeferInvokeCallback,
    // the 0xc000027b fail-fast this file's siblings already fight. The rows live
    // in an ItemsRepeater, so a container gets recycled and re-bound while its
    // ItemsSource binding is still resolving, and assigning a SelectedValue the
    // ComboBox does not yet contain is what throws.
    //
    // Rules kept here: never assign before the items exist, never assign a value
    // that is not in the list, and never let this escape as an unhandled
    // exception — a wrong dropdown is a cosmetic bug, a crash ends the show.
    private void OnProductionRoleComboLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not ComboBox combo || combo.Tag is not FeedHealthRow row)
        {
            return;
        }

        SyncProductionRoleCombo(combo, row);
    }

    private void OnFeedHealthElementPrepared(ItemsRepeater sender, ItemsRepeaterElementPreparedEventArgs args)
    {
        if (args.Element is not FrameworkElement root ||
            FindDescendant<ComboBox>(root, "ProductionRoleCombo") is not { } combo ||
            combo.Tag is not FeedHealthRow row)
        {
            return;
        }

        SyncProductionRoleCombo(combo, row);
    }

    private void SyncProductionRoleCombo(ComboBox combo, FeedHealthRow row)
    {
        try
        {
            // ElementName bindings inside a recycled ItemsRepeater template are
            // not guaranteed to resolve before Loaded. Make the page view-model
            // authoritative and suppress write-back while rebinding the row.
            var options = ViewModel?.ProductionRoleAssignmentOptions;
            if (options is null) return;

            combo.SelectionChanged -= OnProductionRoleChanged;
            combo.ItemsSource = options;
            var roleId = row.ProductionRoleId ?? string.Empty;
            if (!options.Any(option => option.Value == roleId))
            {
                roleId = string.Empty;
            }
            combo.SelectedValue = roleId;
            combo.SelectionChanged += OnProductionRoleChanged;
        }
        catch (Exception ex)
        {
            combo.SelectionChanged -= OnProductionRoleChanged;
            combo.SelectionChanged += OnProductionRoleChanged;
            LaunchLog.Write($"sources: production-role selection skipped ({ex.GetType().Name}: {ex.Message})");
        }
    }

    private void OnProductionRoleChanged(object sender, SelectionChangedEventArgs e)
    {
        // Tag (x:Bind), not DataContext (null inside the ItemsRepeater). The
        // view-model no-ops when the value matches, which also swallows the
        // initial programmatic selection at row realization.
        if (sender is not ComboBox combo ||
            combo.Tag is not FeedHealthRow row ||
            combo.SelectedValue is not string roleId)
        {
            return;
        }

        ViewModel?.SetParticipantProductionRole(row.ParticipantId, roleId);
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
