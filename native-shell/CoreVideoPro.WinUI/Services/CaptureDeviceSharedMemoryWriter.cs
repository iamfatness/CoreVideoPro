using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Threading;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Publishes capture-card BGRA frames into a named shared-memory buffer that the
/// native core maps and reads (see native WinUiCaptureDeviceAdapter). This is the
/// WinUI->core "F1" frame bridge for capture cards: it lets the core composite real
/// capture pixels into the program frame (and therefore recording / streaming),
/// which a base64-over-stdin channel could never sustain at 1080p60.
///
/// Layout (seqlock): [0] uint32 sequence (even = stable, odd = writer mid-update),
/// [4] uint32 width, [8] uint32 height, [12] uint32 reserved, [16..] BGRA bytes.
/// The buffer name embeds the PID and dimensions so each process/resolution gets a
/// fresh mapping (no stale-name collisions; clean re-map on resolution change).
/// </summary>
public static class CaptureDeviceSharedMemoryWriter
{
    private const int HeaderBytes = 16;
    private static readonly object Gate = new();
    private static readonly Dictionary<string, Mapping> Maps = new(StringComparer.Ordinal);
    private static readonly int Pid = Environment.ProcessId;

    public readonly record struct WriteResult(bool MappingChanged, string ShmName, int Width, int Height);

    /// <summary>One live shared buffer, as it must be announced to the core.</summary>
    public readonly record struct LiveMapping(string DeviceId, string ShmName, int Width, int Height);

    /// <summary>
    /// Every mapping currently live, so the caller can RE-ANNOUNCE them to a new core
    /// generation. <see cref="Write"/> reports MappingChanged only when it CREATES a
    /// buffer, so a core that respawns under a live shell never learns about buffers
    /// that already existed: the bridge goes on writing capture pixels at full rate
    /// into shared memory that nobody is reading, and those sources sit on the
    /// placeholder tile forever. Core-side captures (screen/WGC) recover on their own
    /// because the fresh core re-enumerates them; only the shell-bridged cameras are
    /// stranded, which is what makes this look like "some tiles are blank".
    /// </summary>
    public static IReadOnlyList<LiveMapping> LiveMappings()
    {
        lock (Gate)
        {
            var live = new List<LiveMapping>(Maps.Count);
            foreach (var (deviceId, map) in Maps)
            {
                live.Add(new LiveMapping(deviceId, map.Name, map.Width, map.Height));
            }

            return live;
        }
    }

    private sealed class Mapping
    {
        public required MemoryMappedFile File;
        public required MemoryMappedViewAccessor Accessor;
        public required string Name;
        public int Width;
        public int Height;
        public uint Sequence;
    }

    /// <summary>
    /// Writes one BGRA frame into the device's shared buffer (creating/resizing it as
    /// needed). Returns MappingChanged=true when a new buffer was created, so the
    /// caller announces it to the core via register-capture-shm.
    /// </summary>
    public static WriteResult Write(string deviceId, byte[]? bgra, int width, int height)
    {
        if (string.IsNullOrEmpty(deviceId) || bgra is not { Length: > 0 } ||
            width <= 0 || height <= 0 || bgra.Length != (long)width * height * 4)
        {
            return default;
        }

        lock (Gate)
        {
            var changed = false;
            if (!Maps.TryGetValue(deviceId, out var map) || map.Width != width || map.Height != height)
            {
                DisposeMap(map);
                Maps.Remove(deviceId);
                map = CreateMap(deviceId, width, height);
                if (map is null)
                {
                    return default;
                }

                Maps[deviceId] = map;
                changed = true;
            }

            try
            {
                var accessor = map.Accessor;
                accessor.Write(0, map.Sequence + 1u);   // odd: writer mid-update
                Thread.MemoryBarrier();
                accessor.Write(4, (uint)width);
                accessor.Write(8, (uint)height);
                accessor.WriteArray(HeaderBytes, bgra, 0, bgra.Length);
                Thread.MemoryBarrier();
                map.Sequence += 2;                       // even: stable
                accessor.Write(0, map.Sequence);
            }
            catch
            {
                return default;
            }

            return new WriteResult(changed, map.Name, width, height);
        }
    }

    public static void Remove(string deviceId)
    {
        lock (Gate)
        {
            if (Maps.TryGetValue(deviceId, out var map))
            {
                DisposeMap(map);
                Maps.Remove(deviceId);
            }
        }
    }

    private static Mapping? CreateMap(string deviceId, int width, int height)
    {
        try
        {
            var name = $"CoreVideoCaptureShm_{Pid}_{Sanitize(deviceId)}_{width}x{height}";
            long capacity = HeaderBytes + (long)width * height * 4;
            var file = MemoryMappedFile.CreateOrOpen(name, capacity, MemoryMappedFileAccess.ReadWrite);
            var accessor = file.CreateViewAccessor(0, capacity, MemoryMappedFileAccess.ReadWrite);
            return new Mapping { File = file, Accessor = accessor, Name = name, Width = width, Height = height };
        }
        catch
        {
            return null;
        }
    }

    private static void DisposeMap(Mapping? map)
    {
        if (map is null)
        {
            return;
        }

        try { map.Accessor.Dispose(); } catch { /* ignore */ }
        try { map.File.Dispose(); } catch { /* ignore */ }
    }

    private static string Sanitize(string id)
    {
        var chars = id.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
        {
            if (!char.IsLetterOrDigit(chars[i]))
            {
                chars[i] = '_';
            }
        }

        return new string(chars);
    }
}
