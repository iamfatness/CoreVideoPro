namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// #469 / T2.8. Where a recording actually lands.
///
/// The shell's DEFAULT has been an absolute <c>Videos\CoreVideo Pro</c> since
/// 2026-07-21, so "the default is relative" is not what bit the owner on
/// 2026-09-10. What bit is a PERSISTED relative preference: it bypasses the
/// default entirely and is resolved by the CORE against its own working
/// directory, which for an installed build is the install folder. Shows then
/// sit where a version-isolated upgrade or an uninstall can strand or delete
/// them, and the operator cannot find them. Confirmed live 2026-09-12: the
/// stored value was the literal "Recordings/CoreVideo Pro".
///
/// This migrates ACCIDENTS, never DECISIONS: an absolute path an operator
/// chose is returned untouched, including a UNC share.
/// </summary>
public static class RecordingFolderPolicy
{
    public const string DefaultFolderName = "CoreVideo Pro";

    /// <param name="stored">The persisted preference, which may be relative, blank or null.</param>
    /// <param name="videosFolder">The user's Videos folder, or null/blank if it cannot be read.</param>
    /// <returns>An absolute path. Never relative — returning one would silently
    /// put us back where we started.</returns>
    public static string Resolve(string? stored, string? videosFolder)
    {
        // A stripped profile or a service account has no Videos folder. The
        // local application data folder always exists for a user who can run
        // the app at all, so the result is absolute either way.
        var root = string.IsNullOrWhiteSpace(videosFolder)
            ? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                DefaultFolderName)
            : Path.Combine(videosFolder, DefaultFolderName);

        var trimmed = stored?.Trim();
        if (string.IsNullOrEmpty(trimmed))
        {
            return root;
        }

        if (Path.IsPathFullyQualified(trimmed))
        {
            return trimmed;
        }

        // The one legacy value this exists for maps onto the root itself rather
        // than Videos\CoreVideo Pro\Recordings\CoreVideo Pro.
        if (string.Equals(trimmed.Replace('\\', '/'), "Recordings/CoreVideo Pro", StringComparison.OrdinalIgnoreCase))
        {
            return root;
        }

        // Any other relative path keeps the shape the operator typed, under the
        // user folder instead of under the install directory.
        var parent = string.IsNullOrWhiteSpace(videosFolder)
            ? Path.GetDirectoryName(root)!
            : videosFolder;
        return Path.GetFullPath(Path.Combine(parent, trimmed.Replace('/', Path.DirectorySeparatorChar)));
    }
}
