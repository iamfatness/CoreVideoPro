using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class AudioPage : UserControl
{
    public AudioPage()
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
            typeof(AudioPage),
            new PropertyMetadata(null));

    // B5: false when this console is HOSTED IN the pop-out mixer window
    // (hides the recursive "Pop out mixer" affordance).
    public bool ShowPopOutButton
    {
        get => (bool)GetValue(ShowPopOutButtonProperty);
        set => SetValue(ShowPopOutButtonProperty, value);
    }

    public static readonly DependencyProperty ShowPopOutButtonProperty =
        DependencyProperty.Register(
            nameof(ShowPopOutButton),
            typeof(bool),
            typeof(AudioPage),
            new PropertyMetadata(true));

    // B3 strip editing. Same programmatic-echo guard as AudioMixerWindow: row
    // values refresh from snapshots, which re-fires ValueChanged with a value
    // that already matches the model — pushing that back would ping-pong.
    private void OnRowGainChanged(object sender, RangeBaseValueChangedEventArgs args)
    {
        if (!TryResolveParticipantId(sender, out var participantId) ||
            IsProgrammaticMixerValue(participantId, args.NewValue, row => row.ManualGainDb, 0.05))
        {
            return;
        }

        ViewModel?.SetMixerManualGain(participantId, args.NewValue);
    }

    private void OnRowPanChanged(object sender, RangeBaseValueChangedEventArgs args)
    {
        if (!TryResolveParticipantId(sender, out var participantId) ||
            IsProgrammaticMixerValue(participantId, args.NewValue, row => row.Pan, 0.005))
        {
            return;
        }

        ViewModel?.SetMixerPan(participantId, args.NewValue);
    }

    private void OnRowMuteClicked(object sender, RoutedEventArgs e)
    {
        if (TryResolveParticipantId(sender, out var participantId))
        {
            ViewModel?.ToggleMixerMute(participantId);
        }
    }

    private void OnRowSoloClicked(object sender, RoutedEventArgs e)
    {
        if (TryResolveParticipantId(sender, out var participantId))
        {
            ViewModel?.ToggleMixerSolo(participantId);
        }
    }

    // VST host P1: one-shot plugin discovery.
    private void OnScanVstPluginsClicked(object sender, RoutedEventArgs e) =>
        ViewModel?.RequestVstPluginScan();

    private void OnVstBrowserExpanding(Expander sender, ExpanderExpandingEventArgs args) =>
        ViewModel?.EnsureVstPluginScan();

    // ---- C5a insert rack ----------------------------------------------------
    private void OnRemoveInsertSlotClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string insertName })
        {
            ViewModel?.RemoveSelectedChannelInsert(insertName);
        }
    }

    // ---- C5b slot editor: parameter sliders for the built-in processors.
    // (label, key, min, max, step, default) per recognized insert kind — the
    // same names and clamp ranges the core chain applies (AudioDsp.h).
    private static (string Label, string Key, double Min, double Max, double Step, double Default)[] InsertParamSpecs(string insertName)
    {
        var lowered = insertName.ToLowerInvariant();
        if (lowered.Contains("gate") || lowered.Contains("noise"))
        {
            return [("Threshold (dBFS)", "thresholdDb", -80, -12, 1, -48), ("Release (ms)", "releaseMs", 20, 500, 5, 120)];
        }
        if (lowered.Contains("high-pass") || lowered.Contains("highpass") || lowered.Contains("low-cut") || lowered.Contains("hpf"))
        {
            return [("High-pass (Hz)", "highpassHz", 20, 400, 5, 90)];
        }
        if (lowered.Contains("eq") || lowered.Contains("voice"))
        {
            return
            [
                ("High-pass (Hz)", "highpassHz", 20, 400, 5, 90),
                ("Presence (Hz)", "presenceHz", 800, 8000, 100, 3000),
                ("Presence (dB)", "presenceDb", -12, 12, 0.5, 2)
            ];
        }
        if (lowered.Contains("compressor"))
        {
            return [("Threshold (dBFS)", "thresholdDb", -40, -6, 1, -18), ("Ratio", "ratio", 1, 20, 0.5, 4)];
        }
        if (lowered.Contains("limiter"))
        {
            return [("Ceiling (dBFS)", "ceilingDb", -12, -0.1, 0.1, -1)];
        }
        return [];
    }

    private void OnInsertSlotFlyoutOpening(object sender, object e)
    {
        if (sender is not Flyout { Content: StackPanel panel } flyout ||
            flyout.Target is not FrameworkElement target ||
            ViewModel is not { } viewModel)
        {
            return;
        }

        var slot = target.DataContext as Models.InsertSlotItem;
        if (slot is null && target.Tag is string taggedName)
        {
            slot = viewModel.SelectedChannelInsertSlots.FirstOrDefault(candidate =>
                string.Equals(candidate.Name, taggedName, StringComparison.OrdinalIgnoreCase));
        }
        if (slot is null)
        {
            return;
        }

        panel.Children.Clear();
        panel.Children.Add(new TextBlock { Text = slot.Name, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });

        var specs = InsertParamSpecs(slot.Name);
        if (specs.Length == 0)
        {
            if (!slot.IsBuiltIn)
            {
                var openControls = new Button
                {
                    Content = "Open plug-in controls",
                    HorizontalAlignment = HorizontalAlignment.Stretch
                };
                openControls.Click += async (_, _) =>
                {
                    flyout.Hide();
                    await viewModel.OpenVstControlsAsync(slot.Name);
                };
                panel.Children.Add(openControls);

                // A2: generic sliders over the host-published param surface.
                // Rebuilt on every Opening (transient flyout — outside the
                // 0xc000027b bound-collection rules); the host is the value
                // authority, so values here are the host's published truth
                // (editor knob moves land in them too).
                if (BuildVstParamSliders(panel, viewModel, slot.Name))
                {
                    AddRemoveButton(panel, flyout, slot.Name);
                    return;
                }
            }
            panel.Children.Add(new TextBlock
            {
                Text = slot.IsBuiltIn
                    ? "No adjustable parameters."
                    : "Parameters appear here once the plug-in is processing. If the plug-in becomes unavailable, CoreVideo keeps program audio running.",
                FontSize = 10,
                TextWrapping = TextWrapping.Wrap,
                Opacity = 0.7
            });
        }

        var insertName = slot.Name;
        foreach (var spec in specs)
        {
            var slider = new Slider
            {
                Header = spec.Label,
                Minimum = spec.Min,
                Maximum = spec.Max,
                StepFrequency = spec.Step,
                Value = viewModel.GetSelectedChannelInsertParam(insertName, spec.Key, spec.Default)
            };
            var key = spec.Key;
            slider.ValueChanged += (_, args) =>
                ViewModel?.SetSelectedChannelInsertParam(insertName, key, args.NewValue);
            panel.Children.Add(slider);
        }

        AddRemoveButton(panel, flyout, insertName);
    }

    private void AddRemoveButton(StackPanel panel, Flyout flyout, string insertName)
    {
        var remove = new Button { Content = "Remove from chain", Margin = new Thickness(0, 6, 0, 0) };
        remove.Click += (_, _) =>
        {
            ViewModel?.RemoveSelectedChannelInsert(insertName);
            flyout.Hide();
        };
        panel.Children.Add(remove);
    }

    /// <summary>
    /// A2: generic sliders for a VST insert from the host-published param
    /// surface. Returns false when the host has not published params for this
    /// insert yet (plug-in still loading / different active selection), in
    /// which case the caller shows the explanatory text instead.
    /// </summary>
    private bool BuildVstParamSliders(StackPanel panel, StudioViewModel viewModel, string insertName)
    {
        var vstParams = viewModel.VstParamsForInsert(insertName);
        if (vstParams.Count == 0)
        {
            return false;
        }

        var list = new StackPanel { Spacing = 2 };
        foreach (var param in vstParams)
        {
            var headerSuffix = param.Display.Length > 0
                ? $" · {param.Display}{(param.Units.Length > 0 ? " " + param.Units : "")}"
                : param.Units.Length > 0 ? $" · {param.Units}" : "";
            var slider = new Slider
            {
                Header = $"{param.Title}{headerSuffix}",
                Minimum = 0,
                Maximum = 1,
                StepFrequency = param.StepCount > 0 ? 1.0 / param.StepCount : 0.01,
                Value = param.Normalized,
                FontSize = 10
            };
            var paramId = param.Id;
            // Handler attached AFTER the initial Value set: any ValueChanged is
            // a real user gesture, no programmatic-echo ping-pong.
            slider.ValueChanged += (sender, args) =>
            {
                _ = ViewModel?.SetVstInsertParamAsync(insertName, paramId, args.NewValue);
            };
            list.Children.Add(slider);
        }

        panel.Children.Add(new ScrollViewer
        {
            Content = list,
            MaxHeight = 320,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto
        });

        if (viewModel.VstParamTotalCount > vstParams.Count)
        {
            panel.Children.Add(new TextBlock
            {
                Text = $"Showing {vstParams.Count} of {viewModel.VstParamTotalCount} parameters — " +
                       "open the plug-in controls for the full surface.",
                FontSize = 10,
                TextWrapping = TextWrapping.Wrap,
                Opacity = 0.7
            });
        }

        return true;
    }

    // Populate the "Add processing" flyout on open: built-ins first (always
    // live), then every scanned VST3 plugin. Rebuilt each open so a new scan
    // shows up without any binding churn. Section headers are disabled items
    // (MenuFlyout has no native headers).
    private void OnAddInsertFlyoutOpening(object sender, object e)
    {
        if (sender is not MenuFlyout flyout || ViewModel is not { } viewModel)
        {
            return;
        }

        // U1a: opening the add menu is the moment a user is LOOKING for
        // plugins — make sure a scan has happened without them asking.
        viewModel.EnsureVstPluginScan();

        flyout.Items.Clear();
        flyout.Items.Add(new MenuFlyoutItem { Text = "BUILT-IN — processes live", IsEnabled = false });
        foreach (var builtIn in viewModel.BuiltInInsertOptions)
        {
            var item = new MenuFlyoutItem { Text = builtIn, Tag = builtIn };
            item.Click += OnAddBuiltInInsertClicked;
            flyout.Items.Add(item);
        }

        flyout.Items.Add(new MenuFlyoutSeparator());
        flyout.Items.Add(new MenuFlyoutItem { Text = "VST3 PLUG-INS", IsEnabled = false });
        if (viewModel.VstPlugins.Count == 0)
        {
            var hostStatus = viewModel.VstPluginHostSummary;
            flyout.Items.Add(new MenuFlyoutItem
            {
                Text = hostStatus.StartsWith("Scanning", StringComparison.Ordinal) || hostStatus.StartsWith("Looking", StringComparison.Ordinal)
                    ? "Scanning for plugins — reopen this menu in a moment"
                    : "No VST3 plugins found on this machine",
                IsEnabled = false
            });
            return;
        }

        AddValidatedVstItems(flyout, viewModel, OnAddVstInsertClicked);
    }

    private void OnAddBuiltInInsertClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string insertName } && ViewModel is { } viewModel)
        {
            // Toggle adds when absent; if it's already on the chain this is a
            // no-op-with-status via the same path the rack cells use.
            if (!viewModel.SelectedChannelInsertSlots.Any(slot => string.Equals(slot.Name, insertName, StringComparison.OrdinalIgnoreCase)))
            {
                viewModel.ToggleSelectedRackInsert(insertName);
            }
        }
    }

    private void OnAddVstInsertClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string pluginSelection })
        {
            ViewModel?.AddVstInsertToSelectedChannel(pluginSelection);
        }
    }

    private void OnAddProcessingVstFlyoutOpening(object sender, object e)
    {
        if (sender is not MenuFlyout flyout || ViewModel is not { } viewModel)
        {
            return;
        }

        viewModel.EnsureVstPluginScan();
        flyout.Items.Clear();
        AddValidatedVstItems(flyout, viewModel, OnAddProcessingVstInsertClicked);
    }

    private void OnAddProcessingVstInsertClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string pluginSelection })
        {
            ViewModel?.AddVstInsertToSelectedProcessingTarget(pluginSelection);
        }
    }

    private async void OnProcessingSlotClicked(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string insertName } || ViewModel is not { } viewModel)
        {
            return;
        }

        if (insertName.StartsWith("VST:", StringComparison.OrdinalIgnoreCase) ||
            insertName.Contains("VST3", StringComparison.OrdinalIgnoreCase))
        {
            await viewModel.OpenVstControlsAsync(insertName);
            return;
        }

        viewModel.CommandStatus = $"{insertName} is active; adjust it in the master processor above.";
    }

    private void OnRemoveProcessingSlotClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string insertName })
        {
            ViewModel?.RemoveAudioProcessingInsert(insertName);
        }
    }

    private static void AddValidatedVstItems(
        MenuFlyout flyout,
        StudioViewModel viewModel,
        RoutedEventHandler clickHandler)
    {
        var added = 0;
        // Respect the browser search field so large shell bundles (Waves can
        // expose hundreds of classes) do not create an unusable flyout.
        foreach (var plugin in viewModel.FilteredVstPlugins)
        {
            if (!string.Equals(plugin.Probe, "pass", StringComparison.Ordinal) || plugin.ClassNames.Count == 0)
            {
                flyout.Items.Add(new MenuFlyoutItem
                {
                    Text = $"{plugin.Name} · {plugin.ProbeLabel}",
                    IsEnabled = false
                });
                continue;
            }

            foreach (var className in plugin.ClassNames)
            {
                var selection = $"VST:{plugin.Name}/{className}";
                var item = new MenuFlyoutItem
                {
                    Text = string.IsNullOrWhiteSpace(plugin.Vendor)
                        ? $"{className} · {plugin.Name}"
                        : $"{className} · {plugin.Vendor}",
                    Tag = selection
                };
                item.Click += clickHandler;
                flyout.Items.Add(item);
                added++;
            }
        }

        if (added == 0)
        {
            flyout.Items.Add(new MenuFlyoutItem
            {
                Text = viewModel.VstPluginHostSummary,
                IsEnabled = false
            });
        }
    }

    // U2a: bus-card delete. ItemsRepeater templates can't x:Bind a parent
    // command, so the card button reaches the matrix VM via DataContext
    // (the house idiom — see the scene layer cards).
    private void OnRemoveBusCardClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: Models.RoutingBus bus } && ViewModel is { } viewModel)
        {
            viewModel.AudioRoutingMatrix.RemoveBusCommand.Execute(bus);
        }
    }

    // Bus OUTPUT routing: card toggles flip the send through the matrix VM
    // (which owns the exclusive-listen rule and the core sync trigger).
    private void OnBusSendToggleClicked(object sender, RoutedEventArgs e)
    {
        if (sender is Microsoft.UI.Xaml.Controls.Primitives.ToggleButton
            {
                DataContext: Models.RoutingBus bus,
                Tag: string target
            } toggle && ViewModel is { } viewModel)
        {
            viewModel.AudioRoutingMatrix.SetBusSend(bus, target, toggle.IsChecked == true);
        }
    }

    private void OnBusListenClicked(object sender, RoutedEventArgs e)
    {
        if (sender is Microsoft.UI.Xaml.Controls.Primitives.ToggleButton
            {
                DataContext: Models.RoutingBus bus
            } toggle && ViewModel is { } viewModel)
        {
            viewModel.AudioRoutingMatrix.SetListening(bus, toggle.IsChecked == true);
        }
    }

    private void OnBusOutputGainChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (sender.DataContext is Models.RoutingBus bus && ViewModel is { } viewModel &&
            !double.IsNaN(args.NewValue))
        {
            viewModel.AudioRoutingMatrix.SetBusOutputGainDb(bus, args.NewValue);
        }
    }

    // C3: SHOW/SETUP mode buttons (Tag carries the mode).
    private void OnAudioModeClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string mode })
        {
            ViewModel?.SetAudioTabMode(mode);
        }
    }

    // Zoom→program audio topology (owner decision 2026-08-09): per-guest ISO
    // stems vs Zoom's combined program mix. OneWay + Toggled (never TwoWay):
    // the VM re-syncs routing on change, and a bounced snapshot must not
    // re-enter the setter.
    private void OnZoomAudioModeToggled(object sender, RoutedEventArgs e)
    {
        if (sender is ToggleSwitch toggle && ViewModel is { } vm &&
            toggle.IsOn != vm.IsPerGuestIsoAudio)
        {
            vm.SetZoomAudioMode(toggle.IsOn);
        }
    }

    // C2 rack: toggle a built-in processor on the SELECTED channel's insert
    // chain. Tag carries the canonical insert name the core DSP recognizes.
    private void OnRackInsertClicked(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string insertName })
        {
            ViewModel?.ToggleSelectedRackInsert(insertName);
        }
    }

    private bool IsProgrammaticMixerValue(
        string participantId,
        double value,
        Func<AudioParticipantRow, double> selector,
        double tolerance)
    {
        if (!double.IsFinite(value))
        {
            return true;
        }

        var row = ViewModel?.AudioParticipantRows.FirstOrDefault(item =>
            string.Equals(item.Id, participantId, StringComparison.Ordinal));
        return row is not null && Math.Abs(selector(row) - value) <= tolerance;
    }

    private static bool TryResolveParticipantId(object sender, out string participantId)
    {
        participantId = (sender as FrameworkElement)?.Tag as string ?? string.Empty;
        return participantId.Length > 0;
    }
}
