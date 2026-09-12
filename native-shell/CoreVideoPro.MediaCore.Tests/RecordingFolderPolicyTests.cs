using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// #469 / T2.8. The shell's DEFAULT has been an absolute Videos\CoreVideo Pro
// since 2026-07-21, so the issue's "the default is relative" is not what bit.
// What bit is a PERSISTED relative preference: it bypasses the default entirely
// and resolves against the core's working directory, which for an installed
// build is the install folder — where an uninstall or a version-isolated
// upgrade can strand or delete a show. Confirmed live on the owner's machine
// 2026-09-12: the stored value is the literal "Recordings/CoreVideo Pro".
public class RecordingFolderPolicyTests
{
    private const string Videos = @"C:\Users\op\Videos";

    [Fact]
    public void TheLegacyRelativePreferenceIsMigratedToTheUserFolder()
    {
        Assert.Equal(
            Path.Combine(Videos, "CoreVideo Pro"),
            RecordingFolderPolicy.Resolve("Recordings/CoreVideo Pro", Videos));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void NothingStoredMeansTheUserFolder(string? stored)
    {
        Assert.Equal(Path.Combine(Videos, "CoreVideo Pro"), RecordingFolderPolicy.Resolve(stored, Videos));
    }

    // An operator who chose a drive keeps it. This policy migrates accidents,
    // never decisions.
    [Theory]
    [InlineData(@"D:\Shows")]
    [InlineData(@"\\nas\shows\corevideo")]
    public void AnAbsoluteChoiceIsLeftAlone(string stored)
    {
        Assert.Equal(stored, RecordingFolderPolicy.Resolve(stored, Videos));
    }

    // A relative path with structure keeps its structure under the user folder,
    // rather than being flattened to the default.
    [Fact]
    public void ARelativePathKeepsItsShapeUnderTheUserFolder()
    {
        Assert.Equal(
            Path.Combine(Videos, "Shows", "Sunday"),
            RecordingFolderPolicy.Resolve("Shows/Sunday", Videos));
    }

    // No Videos folder (a stripped profile, a service account): the result must
    // still be absolute. Returning the relative value would put us back where
    // we started, silently.
    [Fact]
    public void WithNoUserFolderTheResultIsStillAbsolute()
    {
        var resolved = RecordingFolderPolicy.Resolve("Recordings/CoreVideo Pro", null);
        Assert.True(Path.IsPathFullyQualified(resolved), resolved);
    }

    // A DRIVE-ROOTED path (\Shows) is not fully qualified, but it is still the
    // operator naming a location rather than an accident. It resolves against
    // the current drive and stays absolute; it must never be reparented under
    // Videos, which would silently move a chosen location.
    [Fact]
    public void ADriveRootedChoiceStaysOnItsDrive()
    {
        var resolved = RecordingFolderPolicy.Resolve(@"\Shows\Sunday", Videos);
        Assert.True(Path.IsPathFullyQualified(resolved), resolved);
        Assert.EndsWith(Path.Combine("Shows", "Sunday"), resolved);
        Assert.DoesNotContain("Videos", resolved);
    }
}
