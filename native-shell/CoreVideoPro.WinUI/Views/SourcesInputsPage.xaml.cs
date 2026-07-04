using System.Linq;
using CoreVideoPro.WinUI;
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

        // Resolve the slot view-model by index (DataContext is null for elements realized
        // inside an ItemsRepeater). We set each ComboBox's SelectedValue explicitly here —
        // after the element + its ItemsSource are realized — because the OneWay x:Bind
        // selection does not reliably resolve at realization for the Kind combo (its bound
        // value never gets a post-realization PropertyChanged the way Source does), leaving
        // the TYPE column blank. Setting it with the SelectionChanged handler detached keeps
        // the model authoritative without a spurious write-back.
        var editor = ViewModel?.ShowInputEditors.ElementAtOrDefault(args.Index);

        var kindCombo = FindDescendant<ComboBox>(root, "KindCombo");
        if (kindCombo is null)
        {
            return;
        }

        kindCombo.SelectionChanged -= OnShowInputKindChanged;
        if (editor is not null)
        {
            // Select by the actual option object (SelectedItem), not SelectedValue: a ComboBox
            // SelectedValue bound to an enum + SelectedValuePath does not reliably resolve the
            // selection (string Source values do, enum Kind values render blank). Matching the
            // option instance from the same ItemsSource is deterministic.
            kindCombo.SelectedItem = editor.KindOptions.FirstOrDefault(option => option.Value == editor.Kind);
        }
        kindCombo.SelectionChanged += OnShowInputKindChanged;

        var sourceCombo = FindDescendant<ComboBox>(root, "SourceCombo");
        if (sourceCombo is null)
        {
            return;
        }

        sourceCombo.SelectionChanged -= OnShowInputSourceChanged;
        if (editor is not null && editor.SelectedSourceId is not null)
        {
            sourceCombo.SelectedValue = editor.SelectedSourceId;
        }
        sourceCombo.SelectionChanged += OnShowInputSourceChanged;

        var inputMicCombo = FindDescendant<ComboBox>(root, "InputMicCombo");
        if (inputMicCombo is not null)
        {
            inputMicCombo.SelectionChanged -= OnShowInputMicChanged;
            inputMicCombo.SelectionChanged += OnShowInputMicChanged;
        }

        var captureMicCombo = FindDescendant<ComboBox>(root, "CaptureMicCombo");
        if (captureMicCombo is not null)
        {
            captureMicCombo.SelectionChanged -= OnCaptureDeviceMicChanged;
            captureMicCombo.SelectionChanged += OnCaptureDeviceMicChanged;
        }
    }

    private void OnShowInputKindChanged(object sender, SelectionChangedEventArgs e)
    {
        // Read the selected option object (SelectedItem), not SelectedValue — an enum
        // SelectedValue does not resolve reliably on this ComboBox. Tag (x:Bind), not
        // DataContext (null inside the ItemsRepeater).
        if (sender is not ComboBox combo ||
            combo.Tag is not ShowInputSlotViewModel editor ||
            combo.SelectedItem is not ShowInputKindOption option)
        {
            return;
        }

        var kind = option.Value;
        if (editor.Kind == kind)
        {
            return;
        }

        editor.Kind = kind;
        LaunchLog.Write($"sources: kind={kind} slot={editor.SlotNumber} -> sourceOptions={editor.SourceOptions.Count}");
    }

    private void OnShowInputSourceChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.Tag is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not string sourceId)
        {
            return;
        }

        if (string.Equals(editor.SelectedSourceId, sourceId, System.StringComparison.Ordinal))
        {
            return;
        }

        editor.SelectedSourceId = sourceId;
        LaunchLog.Write($"sources: source selected '{sourceId}' slot={editor.SlotNumber} -> ParticipantId={editor.ParticipantId}");
    }

    private void OnProductionRoleChanged(object sender, SelectionChangedEventArgs e)
    {
        // Tag (x:Bind), not DataContext (null inside the ItemsRepeater). The
        // view-model no-ops when the value matches, which also swallows the
        // initial programmatic selection at row realization.
        if (sender is not ComboBox combo ||
            combo.Tag is not string participantId ||
            combo.SelectedValue is not string roleId)
        {
            return;
        }

        ViewModel?.SetParticipantProductionRole(participantId, roleId);
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

    private void OnCaptureDeviceMicChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.DataContext is not CaptureDevice captureDevice)
        {
            return;
        }

        ViewModel?.SetCaptureDeviceAudioSource(captureDevice.Id, combo.SelectedValue as string);
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
