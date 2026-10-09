using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

// One editor per opening, with a captured participant identity. A recycled or
// reassigned slot must never redirect an already-open operator edit.
internal static class SourcePolicyFlyout
{
    internal static Flyout Create(
        Button owner, ShowInputSlotViewModel editor,
        IReadOnlyList<RouteSelectOption> roles, IReadOnlyList<RouteSelectOption> policies,
        string role, string policy, bool participantAvailable,
        Action<string, string> setRole, Action<string, string> setPolicy)
    {
        var participantId = editor.ParticipantId!;
        var notice = new TextBlock
        {
            Text = participantAvailable ? "Role applies to this meeting. Dropout policy is saved." :
                "Guest is unavailable. Role can be changed when they return; dropout policy is saved.",
            TextWrapping = TextWrapping.Wrap, FontSize = 12
        };
        var roleCombo = MakeCombo("Role", roles, role);
        roleCombo.IsEnabled = participantAvailable;
        var policyCombo = MakeCombo("On dropout", policies, policy);
        var panel = new StackPanel { Width = 300, Spacing = 12 };
        panel.Children.Add(new TextBlock { Text = editor.DisplayName, FontSize = 16, TextWrapping = TextWrapping.Wrap });
        panel.Children.Add(roleCombo);
        panel.Children.Add(policyCombo);
        panel.Children.Add(notice);
        var flyout = new Flyout { Content = panel };

        bool StillTargetsGuest() => ReferenceEquals(owner.Tag, editor) &&
            editor.ShowSourcePolicies && string.Equals(editor.ParticipantId, participantId, StringComparison.Ordinal);
        void Apply(ComboBox combo, Action<string, string> write)
        {
            if (!StillTargetsGuest())
            {
                roleCombo.IsEnabled = policyCombo.IsEnabled = false;
                notice.Text = "Source assignment changed. Close these options and reopen them for the current source.";
                return;
            }
            if (combo.SelectedValue is string value) write(participantId, value);
        }
        // Attach after ItemsSource and selection exist: no initial write-back,
        // no phased SelectedValue binding on a recycled ItemsRepeater container.
        roleCombo.SelectionChanged += (_, _) => Apply(roleCombo, setRole);
        policyCombo.SelectionChanged += (_, _) => Apply(policyCombo, setPolicy);
        return flyout;
    }

    private static ComboBox MakeCombo(string header, IReadOnlyList<RouteSelectOption> options, string selected)
    {
        var combo = new ComboBox
        {
            Header = header, HorizontalAlignment = HorizontalAlignment.Stretch,
            DisplayMemberPath = nameof(RouteSelectOption.Label), SelectedValuePath = nameof(RouteSelectOption.Value),
            ItemsSource = options
        };
        var match = options.FirstOrDefault(option => string.Equals(option.Value, selected, StringComparison.Ordinal));
        // Unknown selections remain unselected, rather than claiming a different
        // saved value. SelectedItem is a member of the already-installed list.
        combo.SelectedItem = match;
        return combo;
    }
}
