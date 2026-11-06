namespace CoreVideoPro.WinUI.Models;

/// <summary>Assignment guidance replaces only a generic empty loading placeholder.</summary>
public static class MultiviewEmptyStatePolicy
{
    public static bool ShowAssignmentGuidance(int tileCount, VideoSurfaceState? surface)
    {
        if (tileCount != 0) return false;
        if (surface is null) return true;
        if (surface.HasFrames || surface.HasPreviewBitmap || surface.AwaitingDirect3D ||
            !string.IsNullOrWhiteSpace(surface.MediaAssetPath)) return false;
        // Unknown or failure statuses remain visible. Never infer that every
        // surface without a picture is merely preparing, or hide its explanation.
        return surface.StatusLine is "Waiting for video frames" or "Preparing video" or "Preparing Program";
    }
}
