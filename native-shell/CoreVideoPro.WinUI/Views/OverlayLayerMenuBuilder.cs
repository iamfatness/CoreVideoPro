using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// POS-2: builds the "Add overlay" flyout — pick a source, then a placement preset
/// (corners w/ safe-area margin, center, free). Shared by the Studio preview header
/// and the Scenes tab so both surfaces stay identical. Rebuilt on every flyout
/// Opening (media assets and options change between opens; a one-shot build like the
/// Add-source flyout would go stale) — an on-demand rebuild of a transient menu, NOT
/// a bound-collection churn, so it is outside the 0xc000027b snapshot-rate rules.
/// </summary>
internal static class OverlayLayerMenuBuilder
{
    public static void Populate(MenuFlyout flyout, StudioViewModel? viewModel)
    {
        flyout.Items.Clear();
        if (viewModel is null)
        {
            return;
        }

        // Media assets first — logos/bugs are the primary overlay case. Empty state is
        // explicit and LOUD (a disabled row), never a silent blank menu.
        // Browser graphics are normally full-canvas alpha overlays. List them by
        // URL/name so the operator never has to remember a generic Show Input slot.
        var browserSources = viewModel.BrowserCaptureDevices;
        if (browserSources.Count > 0)
        {
            var browsers = new MenuFlyoutSubItem { Text = "Browser overlays" };
            foreach (var browser in browserSources)
            {
                browsers.Items.Add(BuildSourceSubMenu(
                    viewModel,
                    browser.Name,
                    $"capture:{browser.Id}"));
            }

            flyout.Items.Add(browsers);
            flyout.Items.Add(new MenuFlyoutSeparator());
        }

        var mediaAssets = viewModel.OverlayMediaAssets;
        if (mediaAssets.Count == 0)
        {
            flyout.Items.Add(new MenuFlyoutItem
            {
                Text = "No media assets - import logos on the Media tab",
                IsEnabled = false
            });
        }
        else
        {
            foreach (var asset in mediaAssets)
            {
                flyout.Items.Add(BuildSourceSubMenu(
                    viewModel,
                    asset.Name,
                    ShowInputRosterService.ToMediaSourceId(asset.Id)));
            }
        }

        flyout.Items.Add(new MenuFlyoutSeparator());

        // Everything the scene "Add source" picker offers is overlay-able too, grouped
        // so the menu stays scannable (10 inputs + live modes + 8 roles).
        var inputs = new MenuFlyoutSubItem { Text = "Show inputs" };
        var live = new MenuFlyoutSubItem { Text = "Live sources" };
        var roles = new MenuFlyoutSubItem { Text = "Roles" };
        foreach (var option in SceneRoutingService.AddSourceOptions)
        {
            if (string.Equals(option.Value, "media", StringComparison.OrdinalIgnoreCase))
            {
                // Covered by the per-asset entries above (no Media-tab selection needed).
                continue;
            }

            var group = option.Value.StartsWith("input-", StringComparison.OrdinalIgnoreCase)
                ? inputs
                : ProductionRoleService.IsRoleOption(option.Value) ? roles : live;
            group.Items.Add(BuildSourceSubMenu(viewModel, option.Label, option.Value));
        }

        flyout.Items.Add(inputs);
        flyout.Items.Add(live);
        flyout.Items.Add(roles);
    }

    private static MenuFlyoutSubItem BuildSourceSubMenu(
        StudioViewModel viewModel,
        string label,
        string optionValue)
    {
        var item = new MenuFlyoutSubItem { Text = label };
        foreach (var placement in OverlayLayerService.PlacementOptions)
        {
            item.Items.Add(new MenuFlyoutItem
            {
                Text = placement.Label,
                Command = viewModel.AddOverlayLayerCommand,
                CommandParameter = $"{placement.Value}|{optionValue}"
            });
        }

        return item;
    }
}
