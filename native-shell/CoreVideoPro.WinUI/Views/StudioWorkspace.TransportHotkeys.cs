using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class StudioWorkspace
{
    public string TransitionDuration(string mode) => mode == "cut" ? "Instant" : "· 300 ms";

    private void AttachTransportHotkeys()
    {
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
}
