using System.Runtime.InteropServices;

namespace CoreVideoPro.WinUI.Services;

internal static class PresentationAttempt
{
    internal const int StillDrawing = unchecked((int)0x887A000A);

    // A deferred or occluded present has not committed this generation. In
    // particular, WAS_STILL_DRAWING must neither invalidate the shared handle
    // nor suppress the next rendering callback's retry.
    internal static bool Commit(int result, Action commit)
    {
        if (result == StillDrawing || result > 0) return false;
        if (result < 0) Marshal.ThrowExceptionForHR(result);
        commit();
        return true;
    }
}
