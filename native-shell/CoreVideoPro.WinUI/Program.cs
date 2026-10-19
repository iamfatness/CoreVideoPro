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
            var inspectProbe = args.Length == 2 && args[0] == "--verify-inspect-video";
            var inspectNativeProbe = args.Length == 3 && args[0] == "--verify-inspect-native";
            var runtimeProbe = args.Length == 2 && args[0] == "--verify-runtime";
            var destinationProbe = args.Length == 2 && args[0] == "--verify-stream-destinations";
            var graphicsProbe = args.Length == 2 && args[0] == "--verify-graphics-workspace";
            var studioProbe = args.Length == 2 && args[0] == "--verify-studio-layout";
            var settingsProbe = studioProbe || graphicsProbe || destinationProbe || (args.Length == 2 && args[0] == "--verify-operator-settings");
            var gradeProbe = args.Length == 3 && args[0] == "--verify-source-grade";
            var meterProbe = args.Length == 3 && args[0] == "--verify-audio-meters";
            if (runtimeProbe || settingsProbe || gradeProbe || meterProbe || inspectProbe || inspectNativeProbe)
                CoreVideoPro.MediaCore.Services.DiagnosticLog.ConfigureVerificationDirectory(
                    Path.Combine(Path.GetDirectoryName(Path.GetFullPath(args[^1]))!, "logs"));
            LaunchLog.Write($"base={AppContext.BaseDirectory}");
            var probeSeconds = meterProbe ? int.Parse(args[1]) : 0;
            if (meterProbe && probeSeconds is < 5 or > 86400)
                throw new ArgumentOutOfRangeException(nameof(args), "Meter probe duration must be 5–86400 seconds.");
#if !COREVIDEO_SELF_CONTAINED
            var options = runtimeProbe || meterProbe || settingsProbe || gradeProbe || inspectProbe || inspectNativeProbe ? Bootstrap.InitializeOptions.None : Bootstrap.InitializeOptions.OnNoMatch_ShowUI;
            if (!Bootstrap.TryInitialize(0x00020004, null, new PackageVersion(), options, out var bootstrapHr))
            {
                LaunchLog.Write($"Bootstrap.TryInitialize failed hr=0x{bootstrapHr:X8}");
                Environment.Exit(bootstrapHr);
            }
#endif

            WinRT.ComWrappersSupport.InitializeComWrappers();

            if (runtimeProbe)
            {
                Services.AlphaRuntimeProbe.Run(args[1]);
                return;
            }

            Application.Start(_ =>
            {
                var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                if (inspectNativeProbe) new App(null, inspectProbe: new Services.SourceInspectVideoProbe(args[2], args[1]));
                else if (inspectProbe) new App(null,inspectProbe: new Services.SourceInspectVideoProbe(args[1]));
                else if (settingsProbe) new App(null, new Services.OperatorSettingsProbe(args[1], destinationProbe, graphicsProbe, studioProbe));
                else if (gradeProbe) new App(null,gradeProbe: new Services.SourceGradeWorkspaceProbe(args[1],args[2]));
                else if (meterProbe) new App(new Services.AudioMeterStressProbe(probeSeconds, args[2]));
                else new App();
            });
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"fatal: {ex}");
            throw;
        }
    }
}
