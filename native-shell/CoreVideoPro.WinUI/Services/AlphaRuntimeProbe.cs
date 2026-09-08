using System.Diagnostics;
using System.Text.Json;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;

namespace CoreVideoPro.WinUI.Services;

// Packaging check: initialize XAML and its theme resources without opening a
// window, restoring a show, touching accounts, or starting the native engine.
internal static class AlphaRuntimeProbe
{
    public static void Run(string reportPath)
    {
        reportPath = Path.GetFullPath(reportPath);
        WritePhase(reportPath, "before-application-start");
        Application.Start(_ =>
        {
            WritePhase(reportPath, "application-start-callback");
            SynchronizationContext.SetSynchronizationContext(new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            new ProbeApplication(reportPath);
        });
    }

    private static void WritePhase(string path, string phase, Exception? error = null, string? detail = null)
    {
        var json = JsonSerializer.Serialize(new { success = false, windowOpened = false, phase, error = error?.ToString(), hresult = error?.HResult, detail });
        File.AppendAllText(path + ".phases.log", json + Environment.NewLine);
        File.WriteAllText(path, json);
    }

    private sealed class ProbeApplication : Application, IXamlMetadataProvider
    {
        // XAML initializes theme types before the first dispatcher callback.
        // Use WinUI type metadata without constructing the product App.
        private readonly Microsoft.UI.Xaml.XamlTypeInfo.XamlControlsXamlMetaDataProvider _metadata = new();
        public IXamlType GetXamlType(Type type) => _metadata.GetXamlType(type);
        public IXamlType GetXamlType(string name) => _metadata.GetXamlType(name);
        public XmlnsDefinition[] GetXmlnsDefinitions() => _metadata.GetXmlnsDefinitions();
        public ProbeApplication(string reportPath)
        {
            WritePhase(reportPath, "application-constructed");
            UnhandledException += (_, args) =>
            {
                WritePhase(reportPath, "xaml-unhandled", args.Exception, args.Message);
                args.Handled = true;
                Environment.ExitCode = 1;
                Exit();
            };
            DispatcherQueue.GetForCurrentThread().TryEnqueue(() => Verify(reportPath));
        }

        private void Verify(string reportPath)
        {
            try
            {
                WritePhase(reportPath, "before-theme-resources");
                Resources.MergedDictionaries.Add(new XamlControlsResources());
                WritePhase(reportPath, "theme-resources-loaded");
                var root = Path.GetFullPath(AppContext.BaseDirectory);
                var modules = Process.GetCurrentProcess().Modules.Cast<ProcessModule>()
                    .Where(module => module.ModuleName.Equals("coreclr.dll", StringComparison.OrdinalIgnoreCase)
                        || module.ModuleName.Equals("Microsoft.UI.Xaml.dll", StringComparison.OrdinalIgnoreCase)
                        || module.ModuleName.Equals("Microsoft.WinUI.dll", StringComparison.OrdinalIgnoreCase)
                        || module.ModuleName.Equals("Microsoft.WindowsAppRuntime.dll", StringComparison.OrdinalIgnoreCase))
                    .Select(module => new { name = module.ModuleName,
                        bundled = Path.GetFullPath(module.FileName).StartsWith(root, StringComparison.OrdinalIgnoreCase) })
                    .ToArray();
                var success = modules.Any(module => module.name.Equals("coreclr.dll", StringComparison.OrdinalIgnoreCase))
                    && modules.Any(module => module.name.Contains("WinUI", StringComparison.OrdinalIgnoreCase)
                        || module.name.Contains("Xaml", StringComparison.OrdinalIgnoreCase))
                    && modules.All(module => module.bundled);
                File.WriteAllText(reportPath, JsonSerializer.Serialize(new { success, windowOpened = false, modules },
                    new JsonSerializerOptions { WriteIndented = true }));
                Environment.ExitCode = success ? 0 : 1;
                DispatcherQueue.GetForCurrentThread().TryEnqueue(Exit);
            }
            catch (Exception error)
            {
                WritePhase(reportPath, "verification-failed", error);
                Environment.ExitCode = 1;
                Exit();
            }
        }
    }
}
