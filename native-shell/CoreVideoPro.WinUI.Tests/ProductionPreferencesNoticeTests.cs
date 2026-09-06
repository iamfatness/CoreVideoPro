using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ProductionPreferencesNoticeTests
{
    [Theory]
    [InlineData(ProductionPreferencesLoadStatus.Missing)]
    [InlineData(ProductionPreferencesLoadStatus.Loaded)]
    public void FirstLaunchAndNormalRestoreDoNotWarn(ProductionPreferencesLoadStatus status) =>
        Assert.Empty(ProductionPreferencesNotice.For(status));

    [Fact]
    public void RecoveryExplainsBackupAndPotentialMissingChanges()
    {
        var message = ProductionPreferencesNotice.For(ProductionPreferencesLoadStatus.Recovered);
        Assert.Contains("backup", message);
        Assert.Contains("Recent changes may be missing", message);
        Assert.DoesNotContain("Default settings were loaded", message);
    }

    [Theory]
    [InlineData(ProductionPreferencesLoadStatus.Corrupt)]
    [InlineData(ProductionPreferencesLoadStatus.Unreadable)]
    public void UnrestoredShowsRequireReviewAndDiscloseDefaults(ProductionPreferencesLoadStatus status)
    {
        var message = ProductionPreferencesNotice.For(status);
        Assert.Contains("Default settings were loaded", message);
        Assert.Contains("before going live", message);
    }
}
