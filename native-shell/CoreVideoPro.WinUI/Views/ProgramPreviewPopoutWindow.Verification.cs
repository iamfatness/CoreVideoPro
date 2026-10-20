namespace CoreVideoPro.WinUI.Views;

public sealed partial class ProgramPreviewPopoutWindow
{
    // Offscreen qualification does not activate a window or steal desktop focus.
    // Window x:Bind normally initializes on Activated (unlike UserControl.Loading).
    internal void InitializeOffscreenBindings() => Bindings.Initialize();
}
