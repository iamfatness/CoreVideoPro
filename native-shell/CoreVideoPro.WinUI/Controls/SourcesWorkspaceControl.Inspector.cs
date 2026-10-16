using CoreVideoPro.WinUI.Views;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Automation;
using static CoreVideoPro.WinUI.Controls.SourcesTableRowControl;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class SourcesWorkspaceControl
{
    private TextBox? _rename;
    private ShowInputSlotViewModel? _renameInput;
    private string? _renameSource;

    private StackPanel BuildInspector()
    {
        var panel = new StackPanel { Spacing = 10 };
        var title = Text("", 17); Bind(title, TextBlock.TextProperty, State, "SelectedInput.OverviewName"); panel.Children.Add(title);
        var headingActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        var grade = Button("Grade…", () => Grade(State.SelectedInput)); grade.Name = "InspectorGradeSource"; Bind(grade, FrameworkElement.TagProperty, State, "SelectedInput.SourceColorGradeId");
        Bind(grade, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.IsAssigned"); headingActions.Children.Add(grade);
        headingActions.Children.Add(Button("Close inspector", () => State.InspectorOpen = false)); panel.Children.Add(headingActions);
        var surface = new VideoSurfaceHost { SourceFit = "fit", Height = 180, IsHitTestVisible = false };
        Bind(surface, VideoSurfaceHost.SurfaceStateProperty, State, "SelectedInput.Inspector.PreviewSurface");
        surface.SetBinding(UIElement.VisibilityProperty, new Binding {Source=State,Path=new PropertyPath("SelectedInput.Inspector.HasPreviewTile"),Converter=new Converters.BoolToVisibilityConverter()});
        panel.Children.Add(new Border { Child = surface, Background = Brush(11, 20, 27), CornerRadius = new(5) });
        var format = Text(""); Bind(format, TextBlock.TextProperty, State, "SelectedInput.Inspector.FormatLabel"); panel.Children.Add(format);
        var freshness = Text("", 11); freshness.TextWrapping = TextWrapping.Wrap;
        Bind(freshness, TextBlock.TextProperty, State, "SelectedInput.Inspector.FormatDetailLabel"); panel.Children.Add(freshness);
        var hint = Text("", 11); hint.TextWrapping = TextWrapping.Wrap; Bind(hint, TextBlock.TextProperty, State, "SelectedInput.AssignmentHint"); panel.Children.Add(hint);
        var tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
        var content = new StackPanel(); panel.Children.Add(tabs); panel.Children.Add(content);
        var panes = new[] { BuildSourceInspector(), BuildPictureInspector(), BuildAudioInspector() };
        var names = new[] { "Source", "Picture / Grade", "Audio" }; var group = "SourceInspector" + Guid.NewGuid().ToString("N");
        for (var i = 0; i < panes.Length; i++)
        {
            var index = i; var tab = new RadioButton { Content = names[i], GroupName = group, FontSize = 11, MinWidth = 0, Padding = new(4), IsChecked = i == 0 };
            tab.Checked += (_, _) => { for (var n = 0; n < panes.Length; n++) panes[n].Visibility = n == index ? Visibility.Visible : Visibility.Collapsed; };
            panes[i].Visibility = i == 0 ? Visibility.Visible : Visibility.Collapsed; tabs.Children.Add(tab); content.Children.Add(panes[i]);
        }
        State.PropertyChanged += (_, args) => { if (args.PropertyName == nameof(SourcesWorkspaceState.SelectedInput) && _renameInput is not null) EndRename(false); };
        return panel;
    }
    private StackPanel BuildSourceInspector()
    {
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(Text("Assigned source", 11));
        Bind(_picker, SourceAssignmentPicker.InputProperty, State, "SelectedInput");
        panel.Children.Add(_picker);
        panel.Children.Add(Text("Source changes use the existing live assignment behavior. Selecting an input alone never changes its assignment.", 11).Wrapped());
        _rename = new TextBox { Header = "Display name", Name = "SelectedSourceName" };
        Bind(_rename, TextBox.TextProperty, State, "SelectedInput.DisplayName");
        Bind(_rename, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.IsDisplayNameEditable");
        _rename.GotFocus += (_, _) =>
        {
            if (_renameInput is not null || State.SelectedInput is not { } input) return;
            _renameInput = input; _renameSource = input.SourceId; var value = _rename.Text;
            _rename.ClearValue(TextBox.TextProperty); _rename.Text = value;
        };
        _rename.LostFocus += (_, _) => EndRename(true); panel.Children.Add(_rename);
        var mv = new CheckBox { Content = "Include in Multiview", MinWidth = 0 };
        Bind(mv, CheckBox.IsCheckedProperty, State, "SelectedInput.InShow", BindingMode.TwoWay);
        Bind(mv, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.IsAssigned"); panel.Children.Add(mv);
        var iso = new CheckBox { Content = "Select ISO source", MinWidth = 0 };
        Bind(iso, CheckBox.IsCheckedProperty, State, "SelectedInput.IsoEnabled", BindingMode.TwoWay);
        Bind(iso, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.ShowIsoToggle"); panel.Children.Add(iso);
        panel.Children.Add(Text("ISO selection is separate from the observed writer state and the recording transport.", 11).Wrapped());
        var rec = Text(""); Bind(rec, TextBlock.TextProperty, State, "SelectedInput.Inspector.RecChipLabel"); panel.Children.Add(rec);
        var lowerThird = Button("Lower-third text…", () =>
        {
            if (State.SelectedInput is not { CanEditLowerThirdText: true } input) return;
            var button = panel.Children.OfType<Button>().First(b => b.Name == "InspectorLowerThirdText");
            button.Tag = input; SourceLowerThirdFlyout.Create(button, input).ShowAt(button);
        });
        lowerThird.Name = "InspectorLowerThirdText"; Bind(lowerThird, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.CanEditLowerThirdText");
        // Bind Tag, not only the click snapshot: existing flyout guards reject a later selected input.
        Bind(lowerThird, FrameworkElement.TagProperty, State, "SelectedInput"); panel.Children.Add(lowerThird);
        var options = Button("Guest role / dropout policy…", () => OpenSourceOptions(panel)); options.Name = "InspectorSourceOptions";
        Bind(options, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.ShowSourcePolicies"); Bind(options, FrameworkElement.TagProperty, State, "SelectedInput"); panel.Children.Add(options);
        var unassign = Button("Unassign this input", () => { if (State.SelectedInput is { } input) Studio?.UnassignShowInputCommand.Execute(input); });
        Bind(unassign, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.IsAssigned"); panel.Children.Add(unassign);
        return panel;
    }
    private void EndRename(bool commit)
    {
        if (_rename is null) return;
        var input = _renameInput; _renameInput = null;
        if (commit && input is not null && ReferenceEquals(State.SelectedInput, input) && input.SourceId == _renameSource)
            input.DisplayName = _rename.Text;
        Bind(_rename, TextBox.TextProperty, State, "SelectedInput.DisplayName"); _renameSource = null;
    }
    private StackPanel BuildPictureInspector()
    {
        var panel = new StackPanel { Spacing = 10 };
        panel.Children.Add(Text("Source grading", 15));
        panel.Children.Add(Text("Open the full native grade workspace for this source: primaries, curves, adjustment stack, LUTs, presets, scopes and drawn ROI. Existing live-edit choice and Apply/Cancel behavior stay intact.", 12).Wrapped());
        var grade = Button("Open grade workspace…", () => Grade(State.SelectedInput));
        Bind(grade, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.IsAssigned"); panel.Children.Add(grade);
        panel.Children.Add(Text("ROI changes scope analysis only. Scene positioning, crop and fit remain in Scene Builder; webcam-only overlays remain in their existing output settings.", 11).Wrapped());
        return panel;
    }
    private StackPanel BuildAudioInspector()
    {
        var panel = new StackPanel { Spacing = 10 };
        panel.Children.Add(Text("Paired audio", 15)); var summary = Text(""); Bind(summary, TextBlock.TextProperty, State, "SelectedInput.AudioSummary"); panel.Children.Add(summary);
        var picker = new DropDownButton { Content = "Choose paired microphone…", HorizontalAlignment = HorizontalAlignment.Stretch };
        Bind(picker, Microsoft.UI.Xaml.Controls.Control.IsEnabledProperty, State, "SelectedInput.ShowAudioDevicePicker");
        var menu = new MenuFlyout(); menu.Opening += (_, _) =>
        {
            menu.Items.Clear(); if (State.SelectedInput is not { } input) return;
            var identity = input.SourceId;
            foreach (var option in input.AudioDeviceOptions)
            {
                var item = new MenuFlyoutItem { Text = option.Label };
                item.Click += (_, _) => { if (ReferenceEquals(input, State.SelectedInput) && input.SourceId == identity) input.AudioDeviceId = option.Value; };
                menu.Items.Add(item);
            }
        };
        picker.Flyout = menu; panel.Children.Add(picker);
        panel.Children.Add(Text("Pairing feeds the existing source channel. Live gain/mute, processing and bus routing remain in Audio/Routing. Device sync offsets remain in the capture catalog.", 12).Wrapped());
        return panel;
    }
    private void OpenSourceOptions(StackPanel panel)
    {
        if (State.SelectedInput is not { ShowSourcePolicies: true } input || Studio is not { } studio) return;
        var button = panel.Children.OfType<Button>().Single(b => b.Name == "InspectorSourceOptions");
        var row = studio.FeedHealthRows.FirstOrDefault(r => r.ParticipantId == input.ParticipantId);
        SourcePolicyFlyout.Create(button, input, studio.ProductionRoleAssignmentOptions, studio.DropoutPolicyOptions,
            row?.ProductionRoleId ?? "", studio.SourceDropoutPolicy("zoom:" + input.ParticipantId), row is not null,
            studio.SetParticipantProductionRole, (id, policy) => studio.SetSourceDropoutPolicy("zoom:" + id, policy)).ShowAt(button);
    }
}

internal static class SourceWorkspaceTextExtensions
{
    internal static TextBlock Wrapped(this TextBlock text) { text.TextWrapping = TextWrapping.Wrap; return text; }
}
