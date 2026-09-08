using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public class ShowEnginePathsTests
{
    private const string AppBaseDir = @"C:\app";
    private const string RepoRoot = @"C:\repo";

    private static Func<string, string?> Env(string? nodeExe = null, string? dir = null) => key => key switch
    {
        ShowEnginePaths.NodeExeEnvVar => nodeExe,
        ShowEnginePaths.ShowEngineDirEnvVar => dir,
        _ => null
    };

    [Fact]
    public void Candidates_BothEnvVarsSet_IncludesEnvCandidateFirst()
    {
        var candidates = ShowEnginePaths.Candidates(
            AppBaseDir, RepoRoot, Env(@"C:\env\node.exe", @"C:\env\engine"), () => null);

        Assert.Equal("env", candidates[0].Source);
        Assert.Equal(@"C:\env\node.exe", candidates[0].NodeExe);
        Assert.Equal(@"C:\env\engine\dist\host\main.js", candidates[0].EntryScript);
    }

    [Fact]
    public void Candidates_OnlyNodeExeEnvVarSet_OmitsEnvCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(
            AppBaseDir, RepoRoot, Env(@"C:\env\node.exe", null), () => null);

        Assert.DoesNotContain(candidates, c => c.Source == "env");
    }

    [Fact]
    public void Candidates_OnlyShowEngineDirEnvVarSet_OmitsEnvCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(
            AppBaseDir, RepoRoot, Env(null, @"C:\env\engine"), () => null);

        Assert.DoesNotContain(candidates, c => c.Source == "env");
    }

    [Fact]
    public void Candidates_NeitherEnvVarSet_OmitsEnvCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(AppBaseDir, RepoRoot, Env(), () => null);

        Assert.DoesNotContain(candidates, c => c.Source == "env");
    }

    [Fact]
    public void Candidates_AlwaysIncludesPackagedCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(AppBaseDir, RepoRoot, Env(), () => null);

        var packaged = Assert.Single(candidates, c => c.Source == "packaged");
        Assert.Equal(Path.Combine(AppBaseDir, "node", "node.exe"), packaged.NodeExe);
        Assert.Equal(Path.Combine(AppBaseDir, "show-engine", "dist", "host", "main.js"), packaged.EntryScript);
    }

    [Fact]
    public void Candidates_NodeOnPathReturnsNull_OmitsDevCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(AppBaseDir, RepoRoot, Env(), () => null);

        Assert.DoesNotContain(candidates, c => c.Source == "dev");
    }

    [Fact]
    public void Candidates_NodeOnPathReturnsPath_IncludesDevCandidate()
    {
        var candidates = ShowEnginePaths.Candidates(AppBaseDir, RepoRoot, Env(), () => @"C:\path\node.exe");

        var dev = Assert.Single(candidates, c => c.Source == "dev");
        Assert.Equal(@"C:\path\node.exe", dev.NodeExe);
        Assert.Equal(Path.Combine(RepoRoot, "show-engine", "dist", "host", "main.js"), dev.EntryScript);
    }

    [Fact]
    public void Candidates_Order_IsEnvThenPackagedThenDev()
    {
        var candidates = ShowEnginePaths.Candidates(
            AppBaseDir, RepoRoot, Env(@"C:\env\node.exe", @"C:\env\engine"), () => @"C:\path\node.exe");

        Assert.Equal(new[] { "env", "packaged", "dev" }, candidates.Select(c => c.Source));
    }

    [Fact]
    public void Resolve_ReturnsFirstCandidateWhoseBothFilesExist()
    {
        var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            Path.Combine(AppBaseDir, "node", "node.exe"),
            Path.Combine(AppBaseDir, "show-engine", "dist", "host", "main.js")
        };

        var resolved = ShowEnginePaths.Resolve(existing.Contains, AppBaseDir, RepoRoot, Env(), () => @"C:\path\node.exe");

        Assert.NotNull(resolved);
        Assert.Equal("packaged", resolved!.Source);
        Assert.Equal(Path.Combine(AppBaseDir, "node", "node.exe"), resolved.NodeExe);
        Assert.Equal(Path.Combine(AppBaseDir, "show-engine", "dist", "host", "main.js"), resolved.EntryScript);
        Assert.Equal(Path.Combine(AppBaseDir, "show-engine"), resolved.WorkingDirectory);
    }

    [Fact]
    public void Resolve_PrefersEnvOverPackagedWhenBothExist()
    {
        var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            @"C:\env\node.exe",
            @"C:\env\engine\dist\host\main.js",
            Path.Combine(AppBaseDir, "node", "node.exe"),
            Path.Combine(AppBaseDir, "show-engine", "dist", "host", "main.js")
        };

        var resolved = ShowEnginePaths.Resolve(
            existing.Contains, AppBaseDir, RepoRoot, Env(@"C:\env\node.exe", @"C:\env\engine"), () => null);

        Assert.NotNull(resolved);
        Assert.Equal("env", resolved!.Source);
        Assert.Equal(@"C:\env\engine", resolved.WorkingDirectory);
    }

    [Fact]
    public void Resolve_DevCandidate_ResolvesWorkingDirectoryToRepoShowEngineDir()
    {
        var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            @"C:\path\node.exe",
            Path.Combine(RepoRoot, "show-engine", "dist", "host", "main.js")
        };

        var resolved = ShowEnginePaths.Resolve(existing.Contains, AppBaseDir, RepoRoot, Env(), () => @"C:\path\node.exe");

        Assert.NotNull(resolved);
        Assert.Equal("dev", resolved!.Source);
        Assert.Equal(Path.Combine(RepoRoot, "show-engine"), resolved.WorkingDirectory);
    }

    [Fact]
    public void Resolve_NoCandidateHasBothFiles_ReturnsNull()
    {
        var resolved = ShowEnginePaths.Resolve(_ => false, AppBaseDir, RepoRoot, Env(), () => null);

        Assert.Null(resolved);
    }

    [Fact]
    public void Resolve_OnlyOneOfTheTwoFilesExists_SkipsThatCandidate()
    {
        var existing = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            Path.Combine(AppBaseDir, "node", "node.exe")
            // main.js missing for packaged
        };

        var resolved = ShowEnginePaths.Resolve(existing.Contains, AppBaseDir, RepoRoot, Env(), () => null);

        Assert.Null(resolved);
    }
}
