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
        var editor = ViewModel?.ShowInputEditors.ElementAtOrDefault(args.Index);

        // SRC-1: one unified source picker per slot (kind inferred from the pick).
        var unifiedCombo = FindDescendant<ComboBox>(root, "UnifiedSourceCombo");
        if (unifiedCombo is null)
        {
            return;
        }

        unifiedCombo.SelectionChanged -= OnShowInputUnifiedSourceChanged;
        if (editor is not null && editor.SelectedUnifiedSourceId is not null)
        {
            unifiedCombo.SelectedValue = editor.SelectedUnifiedSourceId;
        }
        unifiedCombo.SelectionChanged += OnShowInputUnifiedSourceChanged;

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

    private void OnShowInputUnifiedSourceChanged(object sender, SelectionChangedEventArgs e)
    {
        // Tag (x:Bind), not DataContext (null inside the ItemsRepeater). Hint rows
        // ("No media assets — …") are ignored by the view-model setter, which snaps the
        // selection back onto the model value.
        if (sender is not ComboBox combo ||
            combo.Tag is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not string sourceId)
        {
            return;
        }

        if (string.Equals(editor.SelectedUnifiedSourceId, sourceId, System.StringComparison.Ordinal))
        {
            return;
        }

        editor.SelectedUnifiedSourceId = sourceId;
        if (ShowInputRosterService.IsHintSourceId(sourceId))
        {
            // The VM refused the hint row; realign the ComboBox with the model.
            combo.SelectedValue = editor.SelectedUnifiedSourceId;
            return;
        }

        LaunchLog.Write(
            $"sources: source selected '{sourceId}' slot={editor.SlotNumber} -> kind={editor.Kind}");
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
