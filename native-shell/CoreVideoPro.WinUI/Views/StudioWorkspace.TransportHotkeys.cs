using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Input;
using Windows.System;
using Windows.UI.Core;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class StudioWorkspace
{
    public string TransitionDuration(string mode) => mode == "cut" ? "Instant" : "· 300 ms";

    private void AttachTransportHotkeys()
    {
        // Preview events precede a focused button's built-in Space activation.
        // A normal accelerator on the parent loses to the transition flyout button.
        PreviewKeyDown += OnTransportPreviewKeyDown;
        PreviewKeyUp += OnTransportPreviewKeyUp;
        Add(VirtualKey.Enter, VirtualKeyModifiers.Control, false);
        Add(VirtualKey.B, VirtualKeyModifiers.Control | VirtualKeyModifiers.Shift, true);
        void Add(VirtualKey key, VirtualKeyModifiers modifiers, bool blackout)
        {
            var accelerator = new KeyboardAccelerator { Key = key, Modifiers = modifiers };
            accelerator.Invoked += (_, args) =>
            {
                var vm = ViewModel;
                if (vm is null || !vm.IsStudioTab) return;
                var focus = XamlRoot is null ? null : FocusManager.GetFocusedElement(XamlRoot);
                if (focus is TextBox or RichEditBox or AutoSuggestBox or NumberBox or PasswordBox) return;
                var command = blackout ? (System.Windows.Input.ICommand)vm.ToggleProgramBlackoutCommand : vm.TakeCommand;
                if (!command.CanExecute(null)) return;
                args.Handled = true;
                command.Execute(null);
            };
            KeyboardAccelerators.Add(accelerator);
        }
    }

    private bool _spaceTakeCaptured;

    private void OnTransportPreviewKeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key != VirtualKey.Space) return;
        var modified = IsDown(VirtualKey.Control) || IsDown(VirtualKey.Shift) ||
            IsDown(VirtualKey.Menu) || IsDown(VirtualKey.LeftWindows) || IsDown(VirtualKey.RightWindows);
        var focus = XamlRoot is null ? null : FocusManager.GetFocusedElement(XamlRoot) as DependencyObject;
        if (ViewModel is not { IsStudioTab: true } vm || modified || IsTextEntry(focus)) return;
        // Consume even when Take is unavailable: Space must never open a flyout
        // or toggle an output because the focused control happened to change.
        args.Handled = true;
        _spaceTakeCaptured = true;
        if (!args.KeyStatus.WasKeyDown && vm.TakeCommand.CanExecute(null))
            vm.TakeCommand.Execute(null);
    }

    private void OnTransportPreviewKeyUp(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key != VirtualKey.Space || !_spaceTakeCaptured) return;
        args.Handled = true;
        _spaceTakeCaptured = false;
    }

    private static bool IsDown(VirtualKey key) =>
        InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private static bool IsTextEntry(DependencyObject? focus)
    {
        for (var current = focus; current is not null; current = VisualTreeHelper.GetParent(current))
            if (current is TextBox or RichEditBox or AutoSuggestBox or NumberBox or PasswordBox) return true;
        return false;
    }
}
