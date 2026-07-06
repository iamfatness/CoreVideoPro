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
            flyout.Target is not FrameworkElement { DataContext: Models.InsertSlotItem slot } ||
            ViewModel is not { } viewModel)
        {
            return;
        }

        panel.Children.Clear();
        panel.Children.Add(new TextBlock { Text = slot.Name, FontSize = 12, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });

        var specs = InsertParamSpecs(slot.Name);
        if (specs.Length == 0)
        {
            panel.Children.Add(new TextBlock
            {
                Text = slot.IsBuiltIn ? "No adjustable parameters." : "Parameters arrive with the plugin host (P2).",
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

        var remove = new Button { Content = "Remove from chain", Margin = new Thickness(0, 6, 0, 0) };
        remove.Click += (_, _) =>
        {
            ViewModel?.RemoveSelectedChannelInsert(insertName);
            flyout.Hide();
        };
        panel.Children.Add(remove);
    }

    // Populate the "+ Add" flyout on open: built-ins first, then every scanned
    // VST3 plugin (P1 browser results). Rebuilt each open so a new scan shows
    // up without any binding churn.
    private void OnAddInsertFlyoutOpening(object sender, object e)
    {
        if (sender is not MenuFlyout flyout || ViewModel is not { } viewModel)
        {
            return;
        }

        flyout.Items.Clear();
        foreach (var builtIn in viewModel.BuiltInInsertOptions)
        {
            var item = new MenuFlyoutItem { Text = builtIn, Tag = builtIn };
            item.Click += OnAddBuiltInInsertClicked;
            flyout.Items.Add(item);
        }

        if (viewModel.VstPlugins.Count == 0)
        {
            flyout.Items.Add(new MenuFlyoutSeparator());
            flyout.Items.Add(new MenuFlyoutItem { Text = "No VST3 plugins scanned — Scan in SETUP", IsEnabled = false });
            return;
        }

        flyout.Items.Add(new MenuFlyoutSeparator());
        foreach (var plugin in viewModel.VstPlugins)
        {
            var item = new MenuFlyoutItem { Text = plugin.Name + "  (VST3)", Tag = plugin.Name };
            item.Click += OnAddVstInsertClicked;
            flyout.Items.Add(item);
        }
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
        if (sender is FrameworkElement { Tag: string pluginName })
        {
            ViewModel?.AddVstInsertToSelectedChannel(pluginName);
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
