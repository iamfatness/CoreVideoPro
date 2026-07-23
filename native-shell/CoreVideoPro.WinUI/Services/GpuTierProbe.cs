using Vortice.DXGI;
using static Vortice.DXGI.DXGI;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Best-effort, coarse GPU band for the S3 telemetry payload. Enumerates DXGI
/// adapters (Vortice.DXGI, already referenced for the compositor) and returns a
/// NON-identifying band — vendor class + a VRAM bucket, e.g.
/// <c>nvidia-vram16gb+</c> — never a serial, LUID, or per-unit identifier. Fully
/// failure-safe: any exception (or a software-only adapter) yields null, and the
/// telemetry payload is valid without a GPU tier (MediaCore is portable and
/// leaves this to the shell).
/// </summary>
public static class GpuTierProbe
{
    public static string? TryProbe()
    {
        try
        {
            if (CreateDXGIFactory1(out IDXGIFactory1? factory).Failure || factory is null)
            {
                return null;
            }

            using (factory)
            {
                string? best = null;
                double bestVram = -1;

                for (uint i = 0; factory.EnumAdapters1(i, out IDXGIAdapter1? adapter).Success && adapter is not null; i++)
                {
                    using (adapter)
                    {
                        var desc = adapter.Description1;
                        // Skip the Microsoft Basic Render Driver / WARP software adapter.
                        if ((desc.Flags & AdapterFlags.Software) != 0)
                        {
                            continue;
                        }

                        double vram = (double)(ulong)desc.DedicatedVideoMemory;
                        if (vram > bestVram)
                        {
                            bestVram = vram;
                            best = $"{VendorClass(desc.Description)}-{VramBand(vram)}";
                        }
                    }
                }

                return best;
            }
        }
        catch
        {
            return null;
        }
    }

    private static string VendorClass(string? description)
    {
        var text = (description ?? string.Empty).ToLowerInvariant();
        if (text.Contains("nvidia") || text.Contains("geforce") || text.Contains("quadro") || text.Contains("rtx"))
        {
            return "nvidia";
        }

        if (text.Contains("amd") || text.Contains("radeon"))
        {
            return "amd";
        }

        if (text.Contains("intel"))
        {
            return "intel";
        }

        return "other";
    }

    private static string VramBand(double bytes)
    {
        var gb = bytes / (1024.0 * 1024 * 1024);
        return gb switch
        {
            <= 0 => "vram-unknown",
            < 2 => "vram<2gb",
            < 4 => "vram2-4gb",
            < 8 => "vram4-8gb",
            < 12 => "vram8-12gb",
            < 16 => "vram12-16gb",
            _ => "vram16gb+"
        };
    }
}
