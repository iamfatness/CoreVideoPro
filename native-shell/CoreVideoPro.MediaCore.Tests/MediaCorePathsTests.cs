using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaCorePathsTests : IDisposable
{
    private readonly string _repoRoot;
    private readonly Dictionary<string, string?> _originalEnv = new(StringComparer.Ordinal);

    public MediaCorePathsTests()
    {
        _repoRoot = Path.Combine(Path.GetTempPath(), "corevideo-media-core-paths-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_repoRoot);
    }

    public void Dispose()
    {
        foreach (var (key, value) in _originalEnv)
        {
            Environment.SetEnvironmentVariable(key, value);
        }

        if (Directory.Exists(_repoRoot))
        {
            Directory.Delete(_repoRoot, recursive: true);
        }
    }

    [Fact]
    public void BuildZoomSdkArchitectureRootCandidates_PrefersRuntimeOverrideBeforeStagedFolder()
    {
        var staged = Path.Combine(_repoRoot, MediaCorePaths.StagedZoomRuntimeRelativePath);
        var overrideDir = Path.Combine(_repoRoot, "custom-runtime");
        Directory.CreateDirectory(staged);
        Directory.CreateDirectory(overrideDir);

        using var _ = SetEnv(MediaCorePaths.ZoomRuntimeDirEnvVar, overrideDir);

        var candidates = MediaCorePaths.BuildZoomSdkArchitectureRootCandidates(_repoRoot);

        Assert.Equal(overrideDir, candidates[0]);
        Assert.Equal(staged, candidates[1]);
        Assert.Contains(Path.Combine(_repoRoot, MediaCorePaths.ArtifactsZoomRuntimeRelativePath), candidates);
    }

    [Fact]
    public void BuildNativeCoreExecutableCandidates_PrefersDevAdapterBuildBeforeStaleAppCopy()
    {
        var appBase = Path.Combine(_repoRoot, "app");

        var candidates = MediaCorePaths.BuildNativeCoreExecutableCandidates(_repoRoot, appBase);

        Assert.Equal(Path.Combine(_repoRoot, "native", "build-dev", "corevideo-native.exe"), candidates[0]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build-dev", "Release", "corevideo-native.exe"), candidates[1]);
        Assert.Equal(Path.Combine(appBase, "corevideo-native.exe"), candidates[2]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build", "corevideo-native.exe"), candidates[3]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build", "Release", "corevideo-native.exe"), candidates[4]);
    }

    [Fact]
    public void BuildZoomEngineExecutableCandidates_PrefersDevAdapterBuildBeforeStubBuild()
    {
        var appBase = Path.Combine(_repoRoot, "app");

        var candidates = MediaCorePaths.BuildZoomEngineExecutableCandidates(_repoRoot, appBase);

        Assert.Equal(Path.Combine(_repoRoot, "native", "build-dev", "corevideo-zoom-engine.exe"), candidates[0]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build-dev", "Release", "corevideo-zoom-engine.exe"), candidates[1]);
        Assert.Equal(Path.Combine(appBase, "corevideo-zoom-engine.exe"), candidates[2]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build", "corevideo-zoom-engine.exe"), candidates[3]);
        Assert.Equal(Path.Combine(_repoRoot, "native", "build", "Release", "corevideo-zoom-engine.exe"), candidates[4]);
    }

    [Fact]
    public void ResolveZoomSdkArchitectureRoot_ReturnsStagedRuntimeWhenSdkDllPresent()
    {
        var staged = Path.Combine(_repoRoot, MediaCorePaths.StagedZoomRuntimeRelativePath);
        Directory.CreateDirectory(Path.Combine(staged, "bin"));
        Directory.CreateDirectory(Path.Combine(staged, "h"));
        File.WriteAllText(Path.Combine(staged, "bin", "sdk.dll"), "stub");
        File.WriteAllText(Path.Combine(staged, "h", "zoom_sdk.h"), "stub");

        var resolved = MediaCorePaths.ResolveZoomSdkArchitectureRoot(_repoRoot);

        Assert.NotNull(resolved);
        Assert.Contains($"{Path.DirectorySeparatorChar}native-core{Path.DirectorySeparatorChar}zoom-runtime", resolved, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ResolveZoomSdkArchitectureRoot_HonorsZoomSdkDirWhenStagedRuntimeMissing()
    {
        var sdkDir = Path.Combine(_repoRoot, "zoom-sdk");
        Directory.CreateDirectory(Path.Combine(sdkDir, "bin"));
        Directory.CreateDirectory(Path.Combine(sdkDir, "h"));
        File.WriteAllText(Path.Combine(sdkDir, "bin", "sdk.dll"), "stub");
        File.WriteAllText(Path.Combine(sdkDir, "h", "zoom_sdk.h"), "stub");

        using var _ = SetEnv(MediaCorePaths.ZoomSdkDirEnvVar, sdkDir);

        var resolved = MediaCorePaths.ResolveZoomSdkArchitectureRoot(_repoRoot);

        Assert.Equal(Path.GetFullPath(sdkDir), resolved);
    }

    [Fact]
    public void ResolveStagedZoomRuntimeTarget_UsesRuntimeOverrideWhenSet()
    {
        var overrideDir = Path.Combine(_repoRoot, "artifacts", "zoom-runtime");
        Directory.CreateDirectory(overrideDir);

        using var _ = SetEnv(MediaCorePaths.ZoomRuntimeDirEnvVar, overrideDir);

        var target = MediaCorePaths.ResolveStagedZoomRuntimeTarget(_repoRoot);

        Assert.Equal(Path.GetFullPath(overrideDir), target);
    }

    [Fact]
    public void IsZoomOAuthBrokerConfigured_ReturnsTrueWhenBrokerStartUrlSet()
    {
        using var _ = SetEnv(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar, "https://corevideo.example.test/oauth/start");

        Assert.True(MediaCorePaths.IsZoomOAuthBrokerConfigured());
        var manifest = MediaCorePaths.LoadZoomOAuthManifest();
        Assert.Equal("https://corevideo.example.test/oauth/start", manifest.BrokerStartUrl);
        Assert.Equal("https://corevideo.iamfatness.us/oauth/callback", manifest.BrokerCallbackUrl);
        Assert.Equal("y6sIWSwiTZe1JygMx4C9EQ", manifest.PublicClientId);
    }

    [Fact]
    public void IsZoomSdkArchitectureRoot_RequiresBinDllAndHeaderLayout()
    {
        var flatRoot = Path.Combine(_repoRoot, "flat");
        Directory.CreateDirectory(flatRoot);
        File.WriteAllText(Path.Combine(flatRoot, "sdk.dll"), "stub");
        Assert.False(MediaCorePaths.IsZoomSdkArchitectureRoot(flatRoot));

        var binRoot = Path.Combine(_repoRoot, "bin-layout");
        Directory.CreateDirectory(Path.Combine(binRoot, "bin"));
        Directory.CreateDirectory(Path.Combine(binRoot, "h"));
        File.WriteAllText(Path.Combine(binRoot, "bin", "sdk.dll"), "stub");
        File.WriteAllText(Path.Combine(binRoot, "h", "zoom_sdk.h"), "stub");
        Assert.True(MediaCorePaths.IsZoomSdkArchitectureRoot(binRoot));
    }

    [Fact]
    public void NormalizeZoomSdkArchitectureRoot_ResolvesPackageRootToX64Folder()
    {
        var packageRoot = Path.Combine(_repoRoot, "zoom-sdk-windows-7.0.5.39292");
        var architectureRoot = Path.Combine(packageRoot, "x64");
        Directory.CreateDirectory(Path.Combine(architectureRoot, "bin"));
        Directory.CreateDirectory(Path.Combine(architectureRoot, "h"));
        File.WriteAllText(Path.Combine(architectureRoot, "bin", "sdk.dll"), "stub");
        File.WriteAllText(Path.Combine(architectureRoot, "h", "zoom_sdk.h"), "stub");

        var resolved = MediaCorePaths.NormalizeZoomSdkArchitectureRoot(packageRoot);

        Assert.Equal(Path.GetFullPath(architectureRoot), resolved);
    }

    [Fact]
    public void ResolveZoomSdkArchitectureRoot_PrefersRepoZoomSdkBeforeFlatNativeBuildDrop()
    {
        var flatDrop = Path.Combine(_repoRoot, "native", "build-dev");
        Directory.CreateDirectory(flatDrop);
        File.WriteAllText(Path.Combine(flatDrop, "sdk.dll"), "stub");

        var sdkRoot = Path.Combine(_repoRoot, "ZoomSDK", "zoom-sdk-windows-7.0.5.39292", "x64");
        Directory.CreateDirectory(Path.Combine(sdkRoot, "bin"));
        Directory.CreateDirectory(Path.Combine(sdkRoot, "h"));
        File.WriteAllText(Path.Combine(sdkRoot, "bin", "sdk.dll"), "stub");
        File.WriteAllText(Path.Combine(sdkRoot, "h", "zoom_sdk.h"), "stub");

        var resolved = MediaCorePaths.ResolveZoomSdkArchitectureRoot(_repoRoot);

        Assert.Equal(Path.GetFullPath(sdkRoot), resolved);
    }

    private IDisposable SetEnv(string key, string? value)
    {
        _originalEnv.TryAdd(key, Environment.GetEnvironmentVariable(key));
        Environment.SetEnvironmentVariable(key, value);
        return new EnvRestore(key, value, _originalEnv);
    }

    private sealed class EnvRestore : IDisposable
    {
        private readonly string _key;
        private readonly Dictionary<string, string?> _originalEnv;

        public EnvRestore(string key, string? _, Dictionary<string, string?> originalEnv)
        {
            _key = key;
            _originalEnv = originalEnv;
        }

        public void Dispose()
        {
            Environment.SetEnvironmentVariable(_key, _originalEnv[_key]);
        }
    }
}
