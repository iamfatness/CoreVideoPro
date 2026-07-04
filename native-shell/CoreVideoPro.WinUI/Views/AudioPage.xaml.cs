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
