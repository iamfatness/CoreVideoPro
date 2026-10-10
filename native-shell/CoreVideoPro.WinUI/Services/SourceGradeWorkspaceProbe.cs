using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Opt-in real-XAML/native integration host. Owns its synthetic source,
/// offscreen window and core. Never opens production settings or joins a meeting.</summary>
internal sealed class SourceGradeWorkspaceProbe(string corePath,string reportPath)
{
    internal async Task RunAsync()
    {
        var checks = new List<string>(); string? error = null;
        ColorGradeEditorWindow? window = null; GradePreviewCoordinator? coordinator = null;
        var supervisor = new MediaCoreSupervisor(new() { Command=Path.GetFullPath(corePath), MaxRestarts=0,
            Environment=new Dictionary<string,string> { ["COREVIDEO_ISOLATE_MONITORS"]="1", ["COREVIDEO_GPU_CAPTURE"]="0" } });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        var pixels = Enumerable.Range(0,320*180).SelectMany(_=>new byte[] {33,99,177,255}).ToArray();
        var sequence=2u;var sourceName="Local\\CoreVideo-Grade-Probe-"+Guid.NewGuid().ToString("N");
        // Use a named source so the owned core can open this exact mapping.
        using var named = MemoryMappedFile.CreateNew(sourceName,16+pixels.Length);
        using var source = named.CreateViewAccessor();
        source.Write(0,2u);source.Write(4,320);source.Write(8,180);source.WriteArray(16,pixels,0,pixels.Length);
        using var publish = new Timer(_ => { var n=Interlocked.Add(ref sequence,2);source.Write(0,n-1);source.Write(16,n);source.Write(0,n); },null,33,33);
        try {
            await bridge.StartAsync();await bridge.RegisterCaptureShmAsync("grade-probe",sourceName,320,180);
            static NativeMediaCoreCommand Command(string type,object fields) => new() { Type=type,
                ExtensionData=JsonSerializer.Deserialize<Dictionary<string,JsonElement>>(JsonSerializer.Serialize(fields)) };
            await bridge.SyncAsync([
                Command("set-output-profile",new { width=1280,height=720,fps=30 }),
                Command("load-scene-graph",new { sceneId="grade-probe",routes=new[] { new {routeId="source",mode="capture-input",captureDeviceId="grade-probe",fitMode="fill",opacity=1,rect=new {x=0,y=0,width=1,height=1} } } })]);
            var editor = new ColorGradeEditorViewModel("capture:grade-probe","Synthetic grade qualification",new() { Lut="none" });
            editor.ApplyGradeAsync=(grade,epoch,revision)=>bridge.ApplySourceGradeAsync(editor.SourceId,epoch,revision,
                new(grade.Lut,grade.Exposure,grade.Contrast,grade.Saturation,grade.Temperature,grade.Advanced));
            window = new(editor);window.InitializeOffscreenBindings();window.AppWindow.MoveAndResize(new RectInt32(-20000,-20000,1320,920));window.AppWindow.Show(false);
            var dispatcher=window.DispatcherQueue;
            coordinator=new(bridge,editor,action=>dispatcher.TryEnqueue(()=>action()));
            await Eventually(()=>((FrameworkElement)window.Content).IsLoaded && editor.HasPreview,"Native preview/real XAML did not load");checks.Add("native preview opened in real XAML");
            editor.AdvancedExpanded=true;editor.AddCurvesCommand.Execute(null);editor.SetCurve(1,[new(0,0),new(.333,.7),new(1,1)]);
            await Eventually(()=>editor.ScopeSurface.PendingSharedHandle is { IsValid:true },"GPU scopes did not reach real XAML");checks.Add("GPU scopes and numeric curves");
            await editor.ApplyLiveCommand.ExecuteAsync(null);Require(editor.EditStatus.Contains("acknowledged"),editor.EditStatus);checks.Add("native acknowledged apply");
            foreach(var scope in new[]{1,2,3,0}) {editor.ScopeView=scope;await Eventually(()=>editor.ScopeSurface.PendingSharedHandle is { IsValid:true },"Expanded scope did not update");}
            checks.Add("each scope expansion");
            editor.ScopesOriginal=true;editor.CompareOriginal=true;await Eventually(()=>editor.HasPreview && editor.ScopeSurface.PendingSharedHandle is { IsValid:true },"Original comparison did not update");checks.Add("independent original taps");
            var beforeRoi=editor.CopyGradeJson();editor.SetScopeRoi(new(true,.2,.25,.25,.5));
            await Eventually(()=>editor.ScopeSurface.PendingSharedHandle is { IsValid:true } && editor.ScopeStatus.Contains("ROI "),"ROI scopes did not reach the editor");
            Require(editor.CopyGradeJson()==beforeRoi,"ROI modified the grade");
            var guide=Descendants(window.Content).OfType<ScopeRoiControl>().Single();
            Require(guide.Visibility==Visibility.Visible && guide.Roi==editor.ScopeRoi,"ROI guide did not match native measurement request");
            Require(window.Content is FrameworkElement root && root.FindName("RoiPrecisionSettings") is Microsoft.UI.Xaml.Controls.Expander {IsExpanded:false},"ROI precision settings must start collapsed");
            editor.AdvancedExpanded=false;editor.ScopesEnabled=false;window.ArmRoi("circle");
            Require(editor.AdvancedExpanded && editor.ScopesEnabled && guide.DrawingShape=="circle" && guide.Visibility==Visibility.Visible,"Circle tool did not activate scope workspace");
            await Eventually(()=>guide.SourceWidth==320 && guide.SourceHeight==180 && guide.ActualWidth>0 && guide.ActualHeight>0,"Drawing guide did not acquire source geometry");
            var fitted=ScopeRoiGeometry.FitImage(guide.ActualWidth,guide.ActualHeight,320,180);
            Windows.Foundation.Point Position(double x,double y)=>new(fitted.X+x*fitted.Width,fitted.Y+y*fitted.Height);
            Require(guide.BeginDrawingGesture(Position(.2,.2)),"Circle gesture did not start over existing ROI");
            guide.UpdateDrawingGesture(Position(.45,.45));guide.UpdateDrawingGesture(Position(.6,.6));guide.CompleteDrawingGesture();
            // WinRT Point stores float coordinates: compare within subpixel input precision.
            Require(Math.Abs(editor.ScopeRoi.Height-.4)<1e-6,$"Consecutive pointer updates lost source geometry: {editor.ScopeRoi}; source {guide.SourceWidth}x{guide.SourceHeight}");
            await Eventually(()=>editor.ScopeSurface.PendingSharedHandle is {IsValid:true} && editor.ScopeStatus.Contains("Circle ROI"),"Native circle scopes did not reach editor");
            Require(editor.CopyGradeJson()==beforeRoi && Math.Abs(guide.Roi.Width*320-guide.Roi.Height*180)<.01,"Circle ROI was not round or changed grade");
            Require(guide.DrawingShape is null,"Drawing release did not restore selection mode");
            var beforeCancel=editor.ScopeRoi;window.ArmRoi("rectangle");
            Require(guide.BeginDrawingGesture(Position(.3,.3)),"Redraw did not start inside existing selection");
            guide.UpdateDrawingGesture(Position(.7,.7));guide.CompleteDrawingGesture(cancel:true);
            Require(editor.ScopeRoi with {Revision=0}==beforeCancel with {Revision=0},"Cancel did not restore original ROI");
            Require(!guide.BeginDrawingGesture(Position(.95,.95)),"Select mode drew a new ROI outside selection");
            checks.Add("draw tools activate scopes, collapsed precision and native circle mask without changing grade");
            editor.ScopesOriginal=false;await Eventually(()=>editor.ScopeStatus.StartsWith("Graded") && editor.ScopeStatus.Contains("ROI "),"Graded ROI tap failed");
            editor.ClearScopeRoi();await Eventually(()=>editor.ScopeStatus.Contains("Full frame"),"Clear ROI did not restore full-frame scopes");
            checks.Add("ROI native sampling, tap changes, guide and full-frame reset without changing grade");
            editor.CompareOriginal=false;editor.ScopesOriginal=false;
            var preset=editor.CopyGradeJson();editor.ResetAdvancedCommand.Execute(null);editor.UndoGradeCommand.Execute(null);Require(editor.HasAdvancedAdjustments,"Undo lost grade");Require(editor.PasteGradeJson(preset),"Preset reload failed");checks.Add("undo and preset reload");
            window.AppWindow.Resize(new SizeInt32(1100,760));await Task.Delay(250);
            var hosts=Descendants(window.Content).OfType<VideoSurfaceHost>().ToArray();
            await Eventually(()=>hosts.Length==2 && hosts.All(h=>h.SurfaceState?.PendingSharedHandle is { IsValid:true } && h.IsGpuPathActive),"Bound GPU surface hosts did not open native textures");checks.Add("bound GPU surface hosts active");
            Require(Descendants(window.Content).OfType<GradeAdjustmentControl>().Single().Adjustment?.Kind=="curves","Curve control binding did not load");
            Require(Descendants(window.Content).OfType<Microsoft.UI.Xaml.Controls.ComboBox>().Any(c=>c.SelectedItem is GradeOperation),"Stack selection binding did not load");
            Require(hosts.Length==2 && hosts.All(h=>h.ActualWidth>250 && h.ActualHeight>100),"Preview/scope layout collapsed");
            editor.AdvancedExpanded=false;await Task.Delay(100);editor.AdvancedExpanded=true;window.AppWindow.Resize(new SizeInt32(1320,920));await Task.Delay(250);checks.Add("Basic/Advanced and resize layout");
            await Eventually(()=>editor.HasPreview && editor.ScopeSurface.PendingSharedHandle is { IsValid:true },"Final preview did not complete");
            // RenderTargetBitmap proves control layout only; SwapChainPanel pixels
            // cannot be captured by this API. Native pixel tests cover those surfaces.
            var bitmap=new RenderTargetBitmap();await bitmap.RenderAsync((FrameworkElement)window.Content);
            var buffer=await bitmap.GetPixelsAsync();using var reader=DataReader.FromBuffer(buffer);var bytes=new byte[buffer.Length];reader.ReadBytes(bytes);
            var folder=Path.GetDirectoryName(Path.GetFullPath(reportPath))!;Directory.CreateDirectory(folder);
            var directory=await StorageFolder.GetFolderFromPathAsync(folder);
            var image=await directory.CreateFileAsync(Path.GetFileNameWithoutExtension(reportPath)+"-layout.png",CreationCollisionOption.ReplaceExisting);
            using var stream=await image.OpenAsync(FileAccessMode.ReadWrite);
            var encoder=await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId,stream);
            encoder.SetPixelData(BitmapPixelFormat.Bgra8,BitmapAlphaMode.Premultiplied,(uint)bitmap.PixelWidth,(uint)bitmap.PixelHeight,96,96,bytes);
            await encoder.FlushAsync();checks.Add("real XAML layout image (GPU pixels excluded by capture API)");
            coordinator.Dispose();coordinator=null;
            var draft=new ColorGradeEditorViewModel("preview:lower-third","Lower-third draft",new() { Lut="none" });draft.LiveEditing=false;
            draft.SetLowerThirdPreview(new(new() { NameSize=53.25,ShowLogo=false }));
            coordinator=new(bridge,draft,action=>dispatcher.TryEnqueue(()=>action()));
            await Eventually(()=>draft.HasPreview,"Native lower-third draft did not render");
            draft.SetLowerThirdPreview(new(LowerThirdAppearance.FromPreset("minimal-accent")));
            await Eventually(()=>draft.HasPreview,"Native lower-third preset change did not render");
            checks.Add("isolated native lower-third draft and preset replacement");
        } catch(Exception ex) { error=ex.ToString(); }
        finally {
            coordinator?.Dispose();await publish.DisposeAsync();
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath,JsonSerializer.Serialize(new { passed=error is null,checks,error,physicalDisplayPresentationVerified=false },new JsonSerializerOptions { WriteIndented=true }));
            if(error is not null) Environment.ExitCode=1;
            window?.Close();
        }
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root) {
        for(var i=0;i<VisualTreeHelper.GetChildrenCount(root);++i) {var child=VisualTreeHelper.GetChild(root,i);yield return child;foreach(var descendant in Descendants(child)) yield return descendant;}
    }
    private static async Task Eventually(Func<bool> ready,string error) {var clock=Stopwatch.StartNew();while(!ready()) {if(clock.Elapsed>TimeSpan.FromSeconds(30)) throw new InvalidOperationException(error);await Task.Delay(20);}}
    private static void Require(bool ready,string error) {if(!ready) throw new InvalidOperationException(error);}
}
