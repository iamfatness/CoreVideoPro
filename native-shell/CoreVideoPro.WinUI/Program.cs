using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.ApplicationModel.DynamicDependency;

namespace CoreVideoPro.WinUI;

public static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        try
        {
            LaunchLog.Write($"base={AppContext.BaseDirectory}");

            var options = Bootstrap.InitializeOptions.OnNoMatch_ShowUI;
            if (!Bootstrap.TryInitialize(0x00020002, null, new PackageVersion(), options, out var bootstrapHr))
            {
                LaunchLog.Write($"Bootstrap.TryInitialize failed hr=0x{bootstrapHr:X8}");
                Environment.Exit(bootstrapHr);
            }

            WinRT.ComWrappersSupport.InitializeComWrappers();

            Application.Start(_ =>
            {
                var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                new App();
            });
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"fatal: {ex}");
            throw;
        }
    }
}