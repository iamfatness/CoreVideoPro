using System.Diagnostics;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Headless Designer canvas measurement (<c>--verify-designer-preview core.exe report.json</c>).
/// Owns its own core (never the operator's app), opens a pack template in the real
/// <see cref="GraphicsWorkspaceSession"/>, and drives a simulated canvas drag: a stream of
/// <see cref="GraphicsWorkspaceSession.SetLayerNumbers"/> edits at ~120 Hz for ~2 s, each handed to the
/// real <see cref="ColorGradeEditorViewModel"/> and <see cref="GradePreviewCoordinator"/> exactly as
/// <c>GraphicsDesignerControl.UpdatePreviewLease</c> does. It reports distinct native preview frames
/// per second and edit-to-frame latency from <see cref="GradePreviewTimings"/>. "Frame" means the
/// shell received a ready native texture for that revision; on-screen presentation (VideoSurfaceHost,
/// one vsync later at most) is not measured headlessly and is reported as unverified.</summary>
internal static class DesignerPreviewProbe
{
    private const double EditHz = 120, DragSeconds = 2;
    private static readonly string[] Templates = ["corporate-clarity-lower-third", "corporate-clarity-glass-lower-third"];

    internal static async Task RunAsync(string corePath, string reportPath)
    {
        string? error = null; var cases = new List<object>();
        var core = Path.GetFullPath(corePath);
        var environment = new Dictionary<string, string> { ["COREVIDEO_ISOLATE_MONITORS"] = "1", ["COREVIDEO_GPU_CAPTURE"] = "0" };
        if (PacksDirectory(core) is { } packs) environment["COREVIDEO_GRAPHICS_PACKS_DIR"] = packs;
        var supervisor = new MediaCoreSupervisor(new() { Command = core, MaxRestarts = 0, Environment = environment });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        try {
            await bridge.StartAsync();
            static NativeMediaCoreCommand Command(string type, object fields) => new() { Type = type,
                ExtensionData = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(JsonSerializer.Serialize(fields)) };
            await bridge.SyncAsync([Command("set-output-profile", new { width = 1920, height = 1080, fps = 60 })]);
            NativeGraphicsState? graphics = null;
            var wait = Stopwatch.StartNew();
            while (wait.Elapsed < TimeSpan.FromSeconds(30)) {
                graphics = (await bridge.SyncAsync([])).Graphics;
                if (graphics is not null && Templates.All(id => graphics.Templates.Any(t => t.TemplateId == id && t.Definition is not null))) break;
                await Task.Delay(100);
            }
            if (graphics is null || !Templates.All(id => graphics.Templates.Any(t => t.TemplateId == id)))
                throw new InvalidOperationException("Pack templates did not load into the probe core.");
            foreach (var id in Templates) cases.Add(await MeasureAsync(bridge, graphics, graphics.Templates.First(t => t.TemplateId == id)));
        } catch (Exception ex) { error = ex.ToString(); }
        finally {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(new {
                passed = error is null, core, editHz = EditHz, dragSeconds = DragSeconds, cases, error,
                presentationVerified = false, note = "frame = ready native texture observed by the shell; on-screen present not measured" },
                new JsonSerializerOptions { WriteIndented = true }));
            if (error is not null) Environment.ExitCode = 1;
        }
    }

    private static string? PacksDirectory(string core)
    {
        var beside = Path.Combine(Path.GetDirectoryName(core)!, "packs");
        if (Directory.Exists(beside)) return null; // the core's own default
        for (var dir = new DirectoryInfo(Path.GetDirectoryName(core)!); dir is not null; dir = dir.Parent) {
            var candidate = Path.Combine(dir.FullName, "native", "resources", "graphics-packs");
            if (Directory.Exists(candidate)) return candidate;
            candidate = Path.Combine(dir.FullName, "resources", "graphics-packs");
            if (Directory.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static async Task<object> MeasureAsync(MediaCoreBridgeService bridge, NativeGraphicsState graphics, NativeGraphicsTemplate template)
    {
        var editorGate = new object();
        var session = new GraphicsWorkspaceSession((_, _, _) => throw new NotSupportedException("probe sends no graphics commands"), () => bridge.LastSnapshot?.Graphics);
        session.Observe(graphics);
        session.Browse(template);
        var editor = new ColorGradeEditorViewModel("preview:lower-third", "Designer preview probe", new() { Lut = "none" }) { LiveEditing = false };
        using var coordinator = new GradePreviewCoordinator(bridge, editor, action => { lock (editorGate) action(); });
        void Push(bool continuous) {
            lock (editorGate) editor.SetLowerThirdPreview(new(null, Graphics: session.Preview!.ForPreview(null, session.Palette)), continuous);
        }
        Push(false);
        await Until(() => { lock (editorGate) return editor.HasPreview; }, "the first native preview did not arrive");
        var document = session.TemplateDocument ?? throw new InvalidOperationException("template has no editable document");
        var layer = GraphicsTemplateEditor.Layers(document).First(l => !l.Locked &&
            GraphicsTemplateEditor.Layer(document, l.Id).TryGetProperty("x", out var x) && x.ValueKind == JsonValueKind.Number);
        var origin = GraphicsTemplateEditor.Layer(document, layer.Id);
        double x0 = origin.GetProperty("x").GetDouble(), y0 = origin.TryGetProperty("y", out var y) && y.ValueKind == JsonValueKind.Number ? y.GetDouble() : 0;
        await Task.Delay(700); // let the warm-up revision's renewals settle
        coordinator.Timings.Reset();
        var clock = Stopwatch.StartNew();
        session.BeginGesture();
        var period = 1.0 / EditHz; var edits = (int)(EditHz * DragSeconds);
        for (var i = 1; i <= edits; ++i) {
            var due = i * period;
            while (clock.Elapsed.TotalSeconds < due) Thread.Yield();
            // A back-and-forth sweep so successive drafts always differ.
            var step = i % 160 < 80 ? i % 80 : 80 - i % 80;
            session.SetLayerNumbers(layer.Id, new Dictionary<string, double> { ["x"] = x0 + step * 2, ["y"] = y0 });
            Push(true);
        }
        var dragEnd = clock.Elapsed.TotalMilliseconds;
        session.EndGesture();
        long last; lock (editorGate) last = editor.Revision;
        var settle = Stopwatch.StartNew();
        await Until(() => coordinator.Timings.Read().Revisions.Any(r => r.Number == last && r.ReadyAt != 0), "the final drag revision never produced a native frame");
        var snapshot = coordinator.Timings.Read();
        double Ms(long from, long to) => (to - from) * 1000.0 / snapshot.Frequency;
        var start = snapshot.Revisions.Where(r => r.EditedAt != 0).Min(r => r.EditedAt);
        var ready = snapshot.Revisions.Where(r => r.ReadyAt != 0 && r.EditedAt != 0).OrderBy(r => r.ReadyAt).ToArray();
        var duringDrag = ready.Where(r => Ms(start, r.ReadyAt) <= dragEnd).ToArray();
        var gaps = duringDrag.Zip(duringDrag.Skip(1), (a, b) => Ms(a.ReadyAt, b.ReadyAt)).ToArray();
        var finalRow = snapshot.Revisions.First(r => r.Number == last);
        var editToReady = ready.Select(r => Ms(r.EditedAt, r.ReadyAt)).ToArray();
        var sendToReady = ready.Where(r => r.SentAt != 0).Select(r => Ms(r.SentAt, r.ReadyAt)).ToArray();
        var ackMs = ready.Where(r => r.SentAt != 0 && r.AckedAt != 0).Select(r => Ms(r.SentAt, r.AckedAt)).ToArray();
        var core = ready.Where(r => r.Core is not null).Select(r => r.Core!).ToArray();
        return new {
            templateId = template.TemplateId, layer = layer.Id, glass = template.TemplateId.Contains("glass", StringComparison.Ordinal),
            editsDuringDrag = snapshot.Revisions.Count(r => r.EditedAt != 0), dragMs = Math.Round(dragEnd, 1),
            sends = snapshot.Sends, renewals = snapshot.Renewals, failures = snapshot.Failures,
            framesDuringDrag = duringDrag.Length,
            previewFramesPerSecond = Math.Round(duringDrag.Length / (dragEnd / 1000.0), 2),
            editToFrameMs = Stats(editToReady), sendToFrameMs = Stats(sendToReady), sendToAckMs = Stats(ackMs),
            maxFrameGapDuringDragMs = gaps.Length == 0 ? 0 : Math.Round(gaps.Max(), 1),
            lastEditToFrameMs = Math.Round(Ms(finalRow.EditedAt, finalRow.ReadyAt), 1),
            settledAfterDragEndMs = Math.Round(settle.Elapsed.TotalMilliseconds, 1),
            coreAdmittedToReadyMs = Stats(core.Select(c => c.AdmittedToReadyMs).ToArray()),
            corePrepareMs = Stats(core.Select(c => c.PrepareMs).ToArray()),
            coreWorkerMs = Stats(core.Select(c => c.WorkerMs).ToArray()),
        };
    }

    private static object Stats(double[] values)
    {
        if (values.Length == 0) return new { n = 0 };
        var sorted = values.Order().ToArray();
        double At(double q) => sorted[Math.Min(sorted.Length - 1, (int)Math.Round(q * (sorted.Length - 1)))];
        return new { n = sorted.Length, p50 = Math.Round(At(.5), 1), p95 = Math.Round(At(.95), 1), max = Math.Round(sorted[^1], 1) };
    }

    private static async Task Until(Func<bool> ready, string error)
    {
        var clock = Stopwatch.StartNew();
        while (!ready()) { if (clock.Elapsed > TimeSpan.FromSeconds(20)) throw new InvalidOperationException(error); await Task.Delay(5); }
    }
}
