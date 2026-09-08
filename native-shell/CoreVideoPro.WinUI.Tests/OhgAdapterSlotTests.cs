using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OhgAdapterSlotTests
{
    [Fact]
    public void ANewSlotIsEmpty() => Assert.Null(new OhgAdapterSlot().Current);

    [Fact]
    public void ReplaceSwapsTheCurrentAdapter()
    {
        var slot = new OhgAdapterSlot();
        var first = Adapter();
        slot.Replace(first);
        Assert.Same(first, slot.Current);
        slot.Replace(null);
        Assert.Null(slot.Current);
    }

    [Fact]
    public async Task QueuedOldPayloadIsDiscardedAfterSettingsSwap()
    {
        var slot = new OhgAdapterSlot();
        slot.Replace(Adapter());
        var oldBinding = slot.Capture();
        var queue = new SequentialAsyncQueue(_ => { });
        var gate = new TaskCompletionSource();
        var blocked = queue.Enqueue(() => gate.Task);
        OhgHostAdapter? applied = null;
        var queued = queue.Enqueue(() =>
        {
            applied = slot.Resolve(oldBinding, 1, 2);
            return Task.CompletedTask;
        });
        var replacement = Adapter();
        slot.Replace(replacement, 2);
        gate.SetResult();
        await Task.WhenAll(blocked, queued);
        Assert.Null(applied);
        Assert.Same(replacement, slot.Resolve(slot.Capture(), 2, 2));
    }

    [Fact]
    public void OldEngineCannotUseNewBindingBetweenSwapAndRestart()
    {
        var slot = new OhgAdapterSlot();
        var replacement = Adapter();
        slot.Replace(replacement, 2);
        var binding = slot.Capture();
        Assert.Null(slot.Resolve(binding, 1, 1));
        Assert.Same(replacement, slot.Resolve(binding, 2, 2));
    }

    [Fact]
    public void RestartWithoutSettingsSwapRejectsRetiredEngineGeneration()
    {
        var slot = new OhgAdapterSlot();
        slot.Replace(Adapter());
        var binding = slot.Capture();
        Assert.Null(slot.Resolve(binding, 1, 2));
        Assert.Same(slot.Current, slot.Resolve(binding, 2, 2));
    }

    [Fact]
    public void ReplacingEvenTheSameAdapterRetiresQueuedBinding()
    {
        var slot = new OhgAdapterSlot();
        var adapter = Adapter();
        slot.Replace(adapter);
        var retired = slot.Capture();
        slot.Replace(adapter);
        Assert.Null(slot.Resolve(retired, 1, 1));
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
