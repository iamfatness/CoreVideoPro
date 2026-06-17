using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.ApplicationModel.DynamicDependency;

namespace CoreVideoPro.WinUI;

public static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        Bootstrap.Initialize(0x00020002);

        try
        {
            WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(_ =>
            {
                var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                new App();
            });
        }
        finally
        {
            Bootstrap.Shutdown();
        }
    }
}