using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7b Task 10 — the seam behind <c>StudioControlSurface.ReplaceOhgAdapter</c>.
///
/// The surface runs host commands through a <c>SequentialAsyncQueue</c>, so a command can sit
/// enqueued across an <c>await</c> while the operator saves a new show config. The controller
/// ruling is that a link resolves the adapter WHEN IT RUNS, not when it was enqueued — otherwise a
/// command that started queueing before the Save would apply the OLD look→scene presets after the
/// engine has already been restarted onto the new ones, putting the wrong scene on air.
///
/// Capturing the adapter at enqueue time is the mutation this file exists for.
/// </summary>
public sealed class OhgAdapterSlotTests
{
    [Fact]
    public void ANewSlotIsEmpty() => Assert.Null(new OhgAdapterSlot().Current);

    [Fact]
    public void ReplaceSwapsTheCurrentAdapter()
    {
        var slot = new OhgAdapterSlot();
        var first = Adapter();
        var second = Adapter();

        slot.Replace(first);
        Assert.Same(first, slot.Current);

        slot.Replace(second);
        Assert.Same(second, slot.Current);

        slot.Replace(null);
        Assert.Null(slot.Current);
    }

    /// <summary>The whole point: a link that reads <see cref="OhgAdapterSlot.Current"/> at RUN
    /// time sees a replacement made after it was enqueued but before it ran.</summary>
    [Fact]
    public async Task AnEnqueuedLinkSeesTheAdapterThatIsCurrentWhenItRuns()
    {
        var slot = new OhgAdapterSlot();
        var oldAdapter = Adapter();
        var newAdapter = Adapter();
        slot.Replace(oldAdapter);

        var gate = new TaskCompletionSource();
        OhgHostAdapter? seen = null;

        // The link is CREATED while `oldAdapter` is current...
        var link = Task.Run(async () =>
        {
            await gate.Task;
            seen = slot.Current;
        });

        // ...and the swap lands before it is allowed to run.
        slot.Replace(newAdapter);
        gate.SetResult();
        await link;

        Assert.Same(newAdapter, seen);
    }

    [Fact]
    public void TheSurfaceResolvesTheAdapterThroughTheSlot()
    {
        var code = ReadService("StudioControlSurface.cs");

        Assert.Contains("public void ReplaceOhgAdapter(", code, StringComparison.Ordinal);

        // The resolution must happen INSIDE the apply body. Capturing it at enqueue time (a local
        // in OnBridgeHostCommand handed to the link) still mentions the slot elsewhere in the file,
        // so the assertion is scoped to the method that actually applies the command.
        var body = MethodBody(code, "ApplyHostCommandAsync");
        Assert.Contains("_ohgAdapterSlot.Current", body, StringComparison.Ordinal);

        // The old captured field must be gone: a `readonly OhgHostAdapter? _ohgAdapter` cannot be
        // swapped, and a local captured at enqueue time reintroduces the stale-adapter bug.
        Assert.DoesNotContain("readonly OhgHostAdapter? _ohgAdapter;", code, StringComparison.Ordinal);
    }

    /// <summary>The text of a method's body, by brace matching from its declaration.</summary>
    private static string MethodBody(string code, string methodName)
    {
        var declaration = code.IndexOf($"Task {methodName}(", StringComparison.Ordinal);
        Assert.True(declaration >= 0, $"Could not find a declaration for {methodName}.");

        var open = code.IndexOf('{', declaration);
        Assert.True(open >= 0, $"Could not find a body for {methodName}.");

        var depth = 0;
        for (var index = open; index < code.Length; index++)
        {
            if (code[index] == '{') depth++;
            else if (code[index] == '}' && --depth == 0) return code[open..(index + 1)];
        }

        throw new Xunit.Sdk.XunitException($"Unbalanced braces reading the body of {methodName}.");
    }

    private static OhgHostAdapter Adapter()
        => new(
            new SlotFacade(),
            new ShowShellConfig
            {
                DriveHost = true,
                DefaultTransition = "cut",
                Presets = new ShowPresetScenes()
            },
            _ => { });

    private sealed class SlotFacade : IOhgHostFacade
    {
        public int ShowInputCount => 10;
        public bool AssignZoomParticipant(int slot, string? participantId) => true;
        public bool SetInputDisplayName(int slot, string name) => true;
        public bool SetInputLowerThirdTitle(int slot, string title) => true;
        public bool SceneExists(string sceneId) => true;
        public IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots) => Array.Empty<string>();
        public bool CanTake => true;
        public Task<bool> TakeAsync(string transition) => Task.FromResult(true);
        public bool IsKnownTransition(string transition) => true;
        public void SetCaption(string text) { }
        public void ReportStatus(string line) { }
    }

    private static string ReadService(string fileName)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, "native-shell", "CoreVideoPro.WinUI", "Services", fileName);
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }
        }

        throw new FileNotFoundException($"Could not locate {fileName} from the test output directory.");
    }
}
