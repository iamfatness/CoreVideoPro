using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Models
{
    /// <summary>The core's <c>colourSample</c> node (graphics styles slice 3).</summary>
    public sealed record NativeColourSampleState
    {
        public string State { get; init; } = "idle";
        public long Revision { get; init; }
        public string Source { get; init; } = "";
        public long FrameNumber { get; init; }
        public int Width { get; init; }
        public int Height { get; init; }
        public string? BgraBase64 { get; init; }
        public NativeColourSamplePick? LastPick { get; init; }
        public string LastError { get; init; } = "";
    }

    public sealed record NativeColourSamplePick
    {
        public string RequestId { get; init; } = "";
        public long FrameNumber { get; init; }
        public int X { get; init; }
        public int Y { get; init; }
        public string Hex { get; init; } = "";
    }

    /// <summary>A frozen Program thumbnail as the eyedropper shows it: exactly the bytes a pick reads.</summary>
    public sealed record ColourSampleFrame(long FrameNumber, int Width, int Height, byte[] Bgra);
}

namespace CoreVideoPro.MediaCore.Services
{
    /// <summary>What the colour field's eyedropper needs. Implemented over the core by
    /// <see cref="GraphicsColourSampler"/>; tests and probes can supply their own.</summary>
    public interface IColourSampler
    {
        Task<(ColourSampleFrame? Frame, string Reason)> FreezeAsync(CancellationToken cancellation = default);
        Task<(string? Hex, string Reason)> PickAsync(ColourSampleFrame frame, double x, double y, CancellationToken cancellation = default);
        Task ReleaseAsync(CancellationToken cancellation = default);
    }

    /// <summary><c>colour.sample</c> (graphics styles spec 4.2, slice 3): freeze the core's Program
    /// thumbnail readback, then read one sRGB pixel of that frozen copy at a normalized point. The
    /// answer is native truth from the snapshot that answered the command, never a shell-side guess
    /// from a possibly newer image. No OS screen picker and no input injection.</summary>
    public sealed class GraphicsColourSampler : IColourSampler
    {
        private readonly Func<IReadOnlyList<NativeMediaCoreCommand>, CancellationToken, Task<NativeMediaCoreStateSnapshot>> _send;
        private readonly Func<bool> _running;

        public GraphicsColourSampler(Func<bool> running, Func<IReadOnlyList<NativeMediaCoreCommand>, CancellationToken, Task<NativeMediaCoreStateSnapshot>> send)
            => (_running, _send) = (running, send);

        public async Task<(ColourSampleFrame? Frame, string Reason)> FreezeAsync(CancellationToken cancellation = default)
        {
            if (!_running()) return (null, "The media core is not running, so there is no Program frame to sample.");
            var node = (await _send([Command(new() { ["action"] = "freeze", ["source"] = "program" })], cancellation).ConfigureAwait(false)).ColourSample;
            if (node is null) return (null, "This media core cannot sample colours.");
            if (node.State != "frozen" || node.BgraBase64 is not { Length: > 0 } encoded) return (null, Explain(node.LastError));
            byte[] bgra;
            try { bgra = Convert.FromBase64String(encoded); } catch (FormatException) { return (null, "The frozen Program frame was unreadable."); }
            if (node.Width <= 0 || node.Height <= 0 || bgra.Length != node.Width * node.Height * 4) return (null, "The frozen Program frame was unreadable.");
            return (new ColourSampleFrame(node.FrameNumber, node.Width, node.Height, bgra), $"Program frame {node.FrameNumber} frozen ({node.Width}×{node.Height}). Click a pixel.");
        }

        public async Task<(string? Hex, string Reason)> PickAsync(ColourSampleFrame frame, double x, double y, CancellationToken cancellation = default)
        {
            if (!_running()) return (null, "The media core is not running.");
            var requestId = "pick-" + Guid.NewGuid().ToString("N")[..16];
            var node = (await _send([Command(new() { ["action"] = "pick", ["x"] = x, ["y"] = y, ["frameNumber"] = frame.FrameNumber, ["requestId"] = requestId })], cancellation)
                .ConfigureAwait(false)).ColourSample;
            if (node?.LastPick is { } pick && pick.RequestId == requestId && pick.FrameNumber == frame.FrameNumber && GraphicsPalette.TryNormalize(pick.Hex, out _))
                return (pick.Hex, $"Picked {pick.Hex} at {pick.X}, {pick.Y} of the frozen Program frame.");
            return (null, Explain(node?.LastError ?? ""));
        }

        public async Task ReleaseAsync(CancellationToken cancellation = default)
        {
            if (_running()) await _send([Command(new() { ["action"] = "release" })], cancellation).ConfigureAwait(false);
        }

        private static NativeMediaCoreCommand Command(Dictionary<string, object> fields) => new() {
            Type = "colour-sample",
            ExtensionData = fields.ToDictionary(p => p.Key, p => JsonSerializer.SerializeToElement(p.Value), StringComparer.Ordinal)
        };

        public static string Explain(string error) => error switch {
            "colour-sample-no-frame" => "Program has no thumbnail to sample yet. Start the engine and try again.",
            "colour-sample-frame-changed" => "The frozen frame changed. Freeze Program again.",
            "colour-sample-not-frozen" => "Freeze Program before picking.",
            "colour-sample-out-of-frame" => "Click inside the frozen frame.",
            "colour-sample-source-unsupported" => "Only Program can be sampled.",
            "" => "The media core did not answer the pick.",
            _ => "Colour sampling was refused: " + error
        };
    }
}
